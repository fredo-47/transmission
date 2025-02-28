// This file Copyright © 2007-2023 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno> // error codes ERANGE, ...
#include <chrono>
#include <cmath>
#include <cstddef> // std::byte
#include <cstdint>
#include <ctime> // time_t
#include <iterator> // std::back_inserter
#include <optional>
#include <tuple> // std::tie
#include <unordered_map>
#include <utility>
#include <vector>

#include <small/vector.hpp>

#include <fmt/core.h>

#define LIBTRANSMISSION_PEER_MODULE
#include "libtransmission/transmission.h"

#include "libtransmission/announcer.h"
#include "libtransmission/block-info.h" // tr_block_info
#include "libtransmission/clients.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/handshake.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/observable.h"
#include "libtransmission/peer-common.h"
#include "libtransmission/peer-io.h"
#include "libtransmission/peer-mgr-active-requests.h"
#include "libtransmission/peer-mgr-wishlist.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/peer-msgs.h"
#include "libtransmission/peer-socket.h"
#include "libtransmission/quark.h"
#include "libtransmission/session.h"
#include "libtransmission/timer.h"
#include "libtransmission/torrent-magnet.h"
#include "libtransmission/torrent.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/utils.h"
#include "libtransmission/webseed.h"

using namespace std::literals;

static auto constexpr CancelHistorySec = int{ 60 };

// ---

namespace
{

class HandshakeMediator final : public tr_handshake::Mediator
{
private:
    [[nodiscard]] static std::optional<TorrentInfo> torrent(tr_torrent* tor)
    {
        if (tor == nullptr)
        {
            return {};
        }

        auto info = TorrentInfo{};
        info.info_hash = tor->info_hash();
        info.client_peer_id = tor->peer_id();
        info.id = tor->id();
        info.is_done = tor->is_done();
        return info;
    }

public:
    explicit HandshakeMediator(tr_session& session) noexcept
        : session_{ session }
    {
    }

    [[nodiscard]] std::optional<TorrentInfo> torrent(tr_sha1_digest_t const& info_hash) const override
    {
        return torrent(session_.torrents().get(info_hash));
    }

    [[nodiscard]] std::optional<TorrentInfo> torrent_from_obfuscated(tr_sha1_digest_t const& info_hash) const override
    {
        return torrent(tr_torrentFindFromObfuscatedHash(&session_, info_hash));
    }

    [[nodiscard]] bool allows_dht() const override
    {
        return session_.allowsDHT();
    }

    [[nodiscard]] bool allows_tcp() const override
    {
        return session_.allowsTCP();
    }

    void set_utp_failed(tr_sha1_digest_t const& info_hash, tr_socket_address const& socket_address) override;

    [[nodiscard]] libtransmission::TimerMaker& timer_maker() override
    {
        return session_.timerMaker();
    }

    [[nodiscard]] size_t pad(void* setme, size_t maxlen) const override
    {
        auto const len = tr_rand_int(maxlen);
        tr_rand_buffer(setme, len);
        return len;
    }

private:
    tr_session& session_;
};

using Handshakes = std::unordered_map<tr_socket_address, tr_handshake>;

} // anonymous namespace

bool tr_peer_info::is_blocklisted(tr_session const* session) const
{
    if (blocklisted_)
    {
        return *blocklisted_;
    }

    auto const value = session->addressIsBlocked(listen_address());
    blocklisted_ = value;
    return value;
}

#define tr_logAddDebugSwarm(swarm, msg) tr_logAddDebugTor((swarm)->tor, msg)
#define tr_logAddTraceSwarm(swarm, msg) tr_logAddTraceTor((swarm)->tor, msg)

namespace
{

/* better goes first */
constexpr struct
{
    [[nodiscard]] constexpr static int compare(tr_peer_info const& a, tr_peer_info const& b) noexcept // <=>
    {
        if (auto const val = a.compare_by_piece_data_time(b); val != 0)
        {
            return -val;
        }

        if (auto const val = tr_compare_3way(a.from_best(), b.from_best()); val != 0)
        {
            return val;
        }

        return a.compare_by_failure_count(b);
    }

    [[nodiscard]] constexpr bool operator()(tr_peer_info const& a, tr_peer_info const& b) const noexcept
    {
        return compare(a, b) < 0;
    }

    [[nodiscard]] constexpr bool operator()(tr_peer_info const* a, tr_peer_info const* b) const noexcept
    {
        return compare(*a, *b) < 0;
    }
} CompareAtomsByUsefulness{};

} // namespace

/** @brief Opaque, per-torrent data structure for peer connection information */
class tr_swarm
{
public:
    using Peers = std::vector<tr_peerMsgs*>;
    using Pool = std::unordered_map<tr_socket_address, tr_peer_info>;

    [[nodiscard]] auto unique_lock() const
    {
        return tor->unique_lock();
    }

    tr_swarm(tr_peerMgr* manager_in, tr_torrent* tor_in) noexcept
        : manager{ manager_in }
        , tor{ tor_in }
        , tags_{ {
              tor_in->done_.observe([this](tr_torrent*, bool) { on_torrent_done(); }),
              tor_in->doomed_.observe([this](tr_torrent*) { on_torrent_doomed(); }),
              tor_in->got_bad_piece_.observe([this](tr_torrent*, tr_piece_index_t p) { on_got_bad_piece(p); }),
              tor_in->got_metainfo_.observe([this](tr_torrent*) { on_got_metainfo(); }),
              tor_in->piece_completed_.observe([this](tr_torrent*, tr_piece_index_t p) { on_piece_completed(p); }),
              tor_in->started_.observe([this](tr_torrent*) { on_torrent_started(); }),
              tor_in->stopped_.observe([this](tr_torrent*) { on_torrent_stopped(); }),
              tor_in->swarm_is_all_seeds_.observe([this](tr_torrent* /*tor*/) { on_swarm_is_all_seeds(); }),
          } }
    {

        rebuildWebseeds();
    }

    tr_swarm(tr_swarm&&) = delete;
    tr_swarm(tr_swarm const&) = delete;
    tr_swarm& operator=(tr_swarm&&) = delete;
    tr_swarm& operator=(tr_swarm const&) = delete;

    ~tr_swarm()
    {
        auto const lock = unique_lock();
        TR_ASSERT(!is_running);
        TR_ASSERT(std::empty(outgoing_handshakes));
        TR_ASSERT(std::empty(peers));
    }

    [[nodiscard]] bool peer_is_in_use(tr_peer_info const& peer_info) const;

    void cancelOldRequests()
    {
        auto const now = tr_time();
        auto const oldest = now - RequestTtlSecs;

        for (auto const& [block, peer] : active_requests.sentBefore(oldest))
        {
            maybeSendCancelRequest(peer, block, nullptr);
            active_requests.remove(block, peer);
        }
    }

    void cancelAllRequestsForBlock(tr_block_index_t block, tr_peer const* no_notify)
    {
        for (auto* peer : active_requests.remove(block))
        {
            maybeSendCancelRequest(peer, block, no_notify);
        }
    }

    [[nodiscard]] uint16_t countActiveWebseeds(uint64_t now) const noexcept
    {
        if (!tor->is_running() || tor->is_done())
        {
            return {};
        }

        return std::count_if(
            std::begin(webseeds),
            std::end(webseeds),
            [&now](auto const& webseed) { return webseed->isTransferringPieces(now, TR_DOWN, nullptr); });
    }

    [[nodiscard]] TR_CONSTEXPR20 auto peerCount() const noexcept
    {
        return std::size(peers);
    }

    void stop()
    {
        auto const lock = unique_lock();

        is_running = false;
        remove_all_peers();
        outgoing_handshakes.clear();
    }

    void remove_peer(tr_peerMsgs* peer)
    {
        auto const lock = unique_lock();

        auto* const peer_info = peer->peer_info;
        auto const socket_address = peer->socket_address();
        auto const listen_socket_address = peer_info->listen_socket_address();
        auto const was_incoming = peer->is_incoming_connection();
        TR_ASSERT(peer_info != nullptr);

        --stats.peer_count;
        --stats.peer_from_count[peer_info->from_first()];

        if (auto iter = std::find(std::begin(peers), std::end(peers), peer); iter != std::end(peers))
        {
            peers.erase(iter);
            TR_ASSERT(stats.peer_count == peerCount());
        }

        delete peer;

        if (was_incoming)
        {
            [[maybe_unused]] auto const port_empty = std::empty(peer_info->listen_port());
            if (incoming_pool.erase(socket_address) != 0U)
            {
                TR_ASSERT(port_empty);
            }
        }
        graveyard_pool.erase(listen_socket_address);
    }

    void remove_all_peers()
    {
        auto tmp = Peers{};
        std::swap(tmp, peers);
        for (auto* peer : tmp)
        {
            remove_peer(peer);
        }

        TR_ASSERT(stats.peer_count == 0);
    }

    void updateEndgame()
    {
        /* we consider ourselves to be in endgame if the number of bytes
           we've got requested is >= the number of bytes left to download */
        is_endgame_ = uint64_t(std::size(active_requests)) * tr_block_info::BlockSize >= tor->left_until_done();
    }

    [[nodiscard]] constexpr auto isEndgame() const noexcept
    {
        return is_endgame_;
    }

    void addStrike(tr_peerMsgs* peer) const
    {
        tr_logAddTraceSwarm(
            this,
            fmt::format("increasing peer {} strike count to {}", peer->display_name(), peer->strikes + 1));

        if (++peer->strikes >= MaxBadPiecesPerPeer)
        {
            peer->peer_info->ban();
            peer->do_purge = true;
            tr_logAddTraceSwarm(this, fmt::format("banning peer {}", peer->display_name()));
        }
    }

    void rebuildWebseeds()
    {
        auto const n = tor->webseed_count();

        webseeds.clear();
        webseeds.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            webseeds.emplace_back(tr_webseedNew(tor, tor->webseed(i), &tr_swarm::peer_callback_webseed, this));
        }
        webseeds.shrink_to_fit();

        stats.active_webseed_count = 0;
    }

    [[nodiscard]] TR_CONSTEXPR20 auto is_all_seeds() const noexcept
    {
        if (!pool_is_all_seeds_)
        {
            pool_is_all_seeds_ = std::all_of(
                std::begin(connectable_pool),
                std::end(connectable_pool),
                [](auto const& key_val) { return key_val.second.is_seed(); });
        }

        return *pool_is_all_seeds_;
    }

    [[nodiscard]] tr_peer_info* get_existing_peer_info(tr_socket_address const& socket_address) noexcept
    {
        auto&& it = connectable_pool.find(socket_address);
        return it != connectable_pool.end() ? &it->second : nullptr;
    }

    tr_peer_info& ensure_info_exists(
        tr_socket_address const& socket_address,
        uint8_t const flags,
        tr_peer_from const from,
        bool is_connectable)
    {
        TR_ASSERT(socket_address.is_valid());
        TR_ASSERT(from < TR_PEER_FROM__MAX);

        auto&& [it, is_new] = is_connectable ? connectable_pool.try_emplace(socket_address, socket_address, flags, from) :
                                               incoming_pool.try_emplace(socket_address, socket_address.address(), flags, from);
        auto& peer_info = it->second;
        if (!is_new)
        {
            peer_info.found_at(from);
            peer_info.set_pex_flags(flags);
        }

        mark_all_seeds_flag_dirty();

        return peer_info;
    }

    void mark_peer_as_seed(tr_peer_info& peer_info)
    {
        tr_logAddTraceSwarm(this, fmt::format("marking peer {} as a seed", peer_info.display_name()));
        peer_info.set_seed();
        mark_all_seeds_flag_dirty();
    }

    static void peer_callback_webseed(tr_peer* const peer, tr_peer_event const& event, void* const vs)
    {
        TR_ASSERT(peer != nullptr);
        auto* s = static_cast<tr_swarm*>(vs);
        auto const lock = s->unique_lock();

        switch (event.type)
        {
        case tr_peer_event::Type::ClientGotPieceData:
            on_client_got_piece_data(s->tor, event.length, tr_time());
            break;

        default:
            peer_callback_common(peer, event, s);
            break;
        }
    }

    static void peer_callback_bt(tr_peerMsgs* const msgs, tr_peer_event const& event, void* const vs)
    {
        TR_ASSERT(msgs != nullptr);
        auto* s = static_cast<tr_swarm*>(vs);
        TR_ASSERT(msgs->swarm == s);
        auto const lock = s->unique_lock();

        switch (event.type)
        {
        case tr_peer_event::Type::ClientSentPieceData:
            {
                auto const now = tr_time();
                auto* const tor = s->tor;

                tor->uploadedCur += event.length;
                tr_announcerAddBytes(tor, TR_ANN_UP, event.length);
                tor->set_date_active(now);
                tor->set_dirty();
                tor->session->add_uploaded(event.length);

                msgs->peer_info->set_latest_piece_data_time(now);
            }

            break;

        case tr_peer_event::Type::ClientGotPieceData:
            {
                auto const now = tr_time();
                on_client_got_piece_data(s->tor, event.length, now);
                msgs->peer_info->set_latest_piece_data_time(now);
            }

            break;

        case tr_peer_event::Type::ClientGotHave:
        case tr_peer_event::Type::ClientGotHaveAll:
        case tr_peer_event::Type::ClientGotHaveNone:
        case tr_peer_event::Type::ClientGotBitfield:
            /* TODO: if we don't need these, should these events be removed? */
            /* noop */
            break;

        case tr_peer_event::Type::ClientGotChoke:
            s->active_requests.remove(msgs);
            break;

        case tr_peer_event::Type::ClientGotPort:
            if (std::empty(event.port))
            {
                // Do nothing
            }
            // If we don't know the listening port of this peer (i.e. incoming connection and first time ClientGotPort)
            else if (auto const& info = *msgs->peer_info; std::empty(info.listen_port()))
            {
                s->on_got_port(msgs, event, false);
            }
            // If we got a new listening port from a known connectable peer
            else if (info.listen_port() != event.port)
            {
                s->on_got_port(msgs, event, true);
            }

            break;

        case tr_peer_event::Type::ClientGotSuggest:
        case tr_peer_event::Type::ClientGotAllowedFast:
            // not currently supported
            break;

        default:
            peer_callback_common(msgs, event, s);
            break;
        }
    }

    Handshakes outgoing_handshakes;

    mutable tr_swarm_stats stats = {};

    uint8_t optimistic_unchoke_time_scaler = 0;

    bool is_running = false;

    tr_peerMgr* const manager;

    tr_torrent* const tor;

    ActiveRequests active_requests;

    // depends-on: active_requests
    std::vector<std::unique_ptr<tr_peer>> webseeds;

    // depends-on: active_requests
    Peers peers;

    // tr_peerMsgs hold pointers to the items in these containers,
    // therefore references to elements within cannot invalidate
    Pool incoming_pool;
    Pool connectable_pool;
    Pool graveyard_pool;

    tr_peerMsgs* optimistic = nullptr; /* the optimistic peer, or nullptr if none */

    time_t lastCancel = 0;

private:
    static void maybeSendCancelRequest(tr_peer* peer, tr_block_index_t block, tr_peer const* muted)
    {
        auto* msgs = dynamic_cast<tr_peerMsgs*>(peer);
        if (msgs != nullptr && msgs != muted)
        {
            peer->cancels_sent_to_peer.add(tr_time(), 1);
            msgs->cancel_block_request(block);
        }
    }

    void mark_all_seeds_flag_dirty() noexcept
    {
        pool_is_all_seeds_.reset();
    }

    void on_torrent_doomed()
    {
        auto const lock = tor->unique_lock();
        stop();
        tor->swarm = nullptr;
        delete this;
    }

    void on_torrent_done()
    {
        std::for_each(std::begin(peers), std::end(peers), [](auto* const peer) { peer->set_interested(false); });
    }

    void on_swarm_is_all_seeds()
    {
        auto const lock = tor->unique_lock();

        for (auto& [socket_address, atom] : connectable_pool)
        {
            mark_peer_as_seed(atom);
        }

        mark_all_seeds_flag_dirty();
    }

    void on_piece_completed(tr_piece_index_t piece)
    {
        bool piece_came_from_peers = false;

        for (auto* const peer : peers)
        {
            // notify the peer that we now have this piece
            peer->on_piece_completed(piece);

            if (!piece_came_from_peers)
            {
                piece_came_from_peers = peer->blame.test(piece);
            }
        }

        if (piece_came_from_peers) /* webseed downloads don't belong in announce totals */
        {
            tr_announcerAddBytes(tor, TR_ANN_DOWN, tor->piece_size(piece));
        }
    }

    void on_got_bad_piece(tr_piece_index_t piece)
    {
        auto const byte_count = tor->piece_size(piece);

        for (auto* const peer : peers)
        {
            if (peer->blame.test(piece))
            {
                tr_logAddTraceSwarm(
                    this,
                    fmt::format(
                        "peer {} contributed to corrupt piece ({}); now has {} strikes",
                        peer->display_name(),
                        piece,
                        peer->strikes + 1));
                addStrike(peer);
            }
        }

        tr_announcerAddBytes(tor, TR_ANN_CORRUPT, byte_count);
    }

    void on_got_metainfo()
    {
        // the webseed list may have changed...
        rebuildWebseeds();

        // some peer_msgs' progress fields may not be accurate if we
        // didn't have the metadata before now... so refresh them all...
        for (auto* peer : peers)
        {
            peer->onTorrentGotMetainfo();

            if (peer->isSeed())
            {
                mark_peer_as_seed(*peer->peer_info);
            }
        }
    }

    void on_torrent_started();
    void on_torrent_stopped();

    // ---

    static void peer_callback_common(tr_peer* const peer, tr_peer_event const& event, tr_swarm* const s)
    {
        switch (event.type)
        {
        case tr_peer_event::Type::ClientGotRej:
            s->active_requests.remove(s->tor->piece_loc(event.pieceIndex, event.offset).block, peer);
            break;

        case tr_peer_event::Type::ClientGotBlock:
            {
                auto* const tor = s->tor;
                auto const loc = tor->piece_loc(event.pieceIndex, event.offset);
                s->cancelAllRequestsForBlock(loc.block, peer);
                peer->blocks_sent_to_client.add(tr_time(), 1);
                tr_torrentGotBlock(tor, loc.block);
            }

            break;

        case tr_peer_event::Type::Error:
            if (event.err == ERANGE || event.err == EMSGSIZE || event.err == ENOTCONN)
            {
                /* some protocol error from the peer */
                peer->do_purge = true;
                tr_logAddDebugSwarm(
                    s,
                    fmt::format(
                        "setting {} do_purge flag because we got an ERANGE, EMSGSIZE, or ENOTCONN error",
                        peer->display_name()));
            }
            else
            {
                tr_logAddDebugSwarm(s, fmt::format("unhandled error: {}", tr_strerror(event.err)));
            }

            break;

        default:
            TR_ASSERT_MSG(false, "This should be unreachable code");
            break;
        }
    }

    static void on_client_got_piece_data(tr_torrent* const tor, uint32_t const sent_length, time_t const now)
    {
        tor->downloadedCur += sent_length;
        tor->set_date_active(now);
        tor->set_dirty();
        tor->session->add_downloaded(sent_length);
    }

    void on_got_port(tr_peerMsgs* const msgs, tr_peer_event const& event, bool was_connectable)
    {
        auto& info_this = *msgs->peer_info;
        TR_ASSERT(info_this.is_connected());
        TR_ASSERT(was_connectable != std::empty(info_this.listen_port()));

        // If we already know about this peer, merge the info objects without invalidating references
        if (auto it_that = connectable_pool.find({ info_this.listen_address(), event.port });
            it_that != std::end(connectable_pool))
        {
            auto& info_that = it_that->second;
            TR_ASSERT(it_that->first == info_that.listen_socket_address());
            TR_ASSERT(it_that->first.address() == info_this.listen_address());
            TR_ASSERT(it_that->first.port() != info_this.listen_port());

            // If there is an existing connection to this peer, keep the better one
            if (info_that.is_connected() && on_got_port_duplicate_connection(msgs, it_that, was_connectable))
            {
                return;
            }

            info_this.merge(info_that);
            connectable_pool.erase(info_that.listen_socket_address());
        }
        else if (!was_connectable)
        {
            info_this.set_connectable();
        }

        auto nh = was_connectable ? connectable_pool.extract(info_this.listen_socket_address()) :
                                    incoming_pool.extract(msgs->socket_address());
        TR_ASSERT(!std::empty(nh));
        if (was_connectable)
        {
            TR_ASSERT(nh.key() == nh.mapped().listen_socket_address());
        }
        else
        {
            TR_ASSERT(nh.key().address() == nh.mapped().listen_address());
        }
        nh.key().port_ = event.port;
        [[maybe_unused]] auto const inserted = connectable_pool.insert(std::move(nh)).inserted;
        TR_ASSERT(inserted);
        info_this.set_listen_port(event.port);

        mark_all_seeds_flag_dirty();
    }

    bool on_got_port_duplicate_connection(tr_peerMsgs* const msgs, Pool::iterator& it_that, bool was_connectable)
    {
        auto& info_this = *msgs->peer_info;
        auto& info_that = it_that->second;

        TR_ASSERT(info_that.is_connected());

        if (CompareAtomsByUsefulness(info_this, info_that))
        {
            auto it = std::find_if(
                std::begin(peers),
                std::end(peers),
                [&info_that](tr_peerMsgs const* const peer) { return peer->peer_info == &info_that; });
            TR_ASSERT(it != std::end(peers));
            (*it)->do_purge = true;

            if (was_connectable)
            {
                // Note that it_that is invalid after this point
                graveyard_pool.insert(connectable_pool.extract(it_that));
            }

            return false;
        }

        info_that.merge(info_this);
        msgs->do_purge = true;

        if (was_connectable)
        {
            graveyard_pool.insert(connectable_pool.extract(info_this.listen_socket_address()));
        }

        mark_all_seeds_flag_dirty();
        return true;
    }

    // ---

    // number of bad pieces a peer is allowed to send before we ban them
    static auto constexpr MaxBadPiecesPerPeer = int{ 5 };

    // how long we'll let requests we've made linger before we cancel them
    static auto constexpr RequestTtlSecs = int{ 90 };

    std::array<libtransmission::ObserverTag, 8> const tags_;

    mutable std::optional<bool> pool_is_all_seeds_;

    bool is_endgame_ = false;
};

struct tr_peerMgr
{
private:
    static auto constexpr BandwidthTimerPeriod = 500ms;
    static auto constexpr RechokePeriod = 10s;
    static auto constexpr RefillUpkeepPeriod = 10s;

    // Max number of outbound peer connections to initiate.
    // This throttle is an arbitrary number to avoid overloading routers.
    static auto constexpr MaxConnectionsPerSecond = size_t{ 18U };
    static auto constexpr MaxConnectionsPerPulse = size_t(MaxConnectionsPerSecond * BandwidthTimerPeriod / 1s);

    // Building a peer candidate list is expensive, so cache it across pulses.
    // We want to cache it long enough to avoid excess CPU cycles,
    // but short enough that the data isn't too stale.
    static auto constexpr OutboundCandidatesListTtl = BandwidthTimerPeriod * 4U;

    // How big the candidate list should be when we create it.
    static auto constexpr OutboundCandidateListCapacity = MaxConnectionsPerPulse * OutboundCandidatesListTtl /
        BandwidthTimerPeriod;

public:
    // The peers we might try connecting to in the next few seconds.
    // This list is cached between pulses so use resilient keys, e.g.
    // a `tr_torrent_id_t` instead of a `tr_torrent*` that can be freed.
    using OutboundCandidates = small::
        max_size_vector<std::pair<tr_torrent_id_t, tr_socket_address>, OutboundCandidateListCapacity>;

    explicit tr_peerMgr(tr_session* session_in)
        : session{ session_in }
        , handshake_mediator_{ *session }
        , bandwidth_timer_{ session->timerMaker().create([this]() { bandwidthPulse(); }) }
        , rechoke_timer_{ session->timerMaker().create([this]() { rechokePulseMarshall(); }) }
        , refill_upkeep_timer_{ session->timerMaker().create([this]() { refillUpkeep(); }) }
        , blocklist_tag_{ session->blocklist_changed_.observe([this]() { on_blocklist_changed(); }) }
    {
        bandwidth_timer_->start_repeating(BandwidthTimerPeriod);
        rechoke_timer_->start_repeating(RechokePeriod);
        refill_upkeep_timer_->start_repeating(RefillUpkeepPeriod);
    }

    tr_peerMgr(tr_peerMgr&&) = delete;
    tr_peerMgr(tr_peerMgr const&) = delete;
    tr_peerMgr& operator=(tr_peerMgr&&) = delete;
    tr_peerMgr& operator=(tr_peerMgr const&) = delete;

    [[nodiscard]] auto unique_lock() const
    {
        return session->unique_lock();
    }

    ~tr_peerMgr()
    {
        auto const lock = unique_lock();
        incoming_handshakes.clear();
    }

    void rechokeSoon() noexcept
    {
        rechoke_timer_->set_interval(100ms);
    }

    void bandwidthPulse();
    void rechokePulse() const;
    void reconnectPulse();
    void refillUpkeep() const;
    void make_new_peer_connections();

    [[nodiscard]] tr_swarm* get_existing_swarm(tr_sha1_digest_t const& hash) const
    {
        auto* const tor = session->torrents().get(hash);
        return tor == nullptr ? nullptr : tor->swarm;
    }

    tr_session* const session;
    Handshakes incoming_handshakes;

    HandshakeMediator handshake_mediator_;

private:
    void rechokePulseMarshall()
    {
        rechokePulse();
        rechoke_timer_->set_interval(RechokePeriod);
    }

    void on_blocklist_changed() const
    {
        /* we cache whether or not a peer is blocklisted...
           since the blocklist has changed, erase that cached value */
        for (auto* const tor : session->torrents())
        {
            for (auto& pool : { std::ref(tor->swarm->connectable_pool), std::ref(tor->swarm->incoming_pool) })
            {
                for (auto& [socket_address, atom] : pool.get())
                {
                    atom.set_blocklisted_dirty();
                }
            }
        }
    }

    OutboundCandidates outbound_candidates_;

    std::unique_ptr<libtransmission::Timer> const bandwidth_timer_;
    std::unique_ptr<libtransmission::Timer> const rechoke_timer_;
    std::unique_ptr<libtransmission::Timer> const refill_upkeep_timer_;

    libtransmission::ObserverTag const blocklist_tag_;
};

// --- tr_peer virtual functions

tr_peer::tr_peer(tr_torrent const* tor)
    : session{ tor->session }
    , swarm{ tor->swarm }
    , blame{ tor->block_count() }
{
}

tr_peer::~tr_peer()
{
    if (swarm != nullptr)
    {
        swarm->active_requests.remove(this);
    }
}

// ---

tr_peerMgr* tr_peerMgrNew(tr_session* session)
{
    return new tr_peerMgr{ session };
}

void tr_peerMgrFree(tr_peerMgr* manager)
{
    delete manager;
}

// ---

/**
 * REQUESTS
 *
 * There are two data structures associated with managing block requests:
 *
 * 1. tr_swarm::active_requests, an opaque class that tracks what requests
 *    we currently have, i.e. which blocks and from which peers.
 *    This is used for cancelling requests that have been waiting
 *    for too long and avoiding duplicate requests.
 *
 * 2. tr_swarm::pieces, an array of "struct weighted_piece" which lists the
 *    pieces that we want to request. It's used to decide which blocks to
 *    return next when tr_peerMgrGetBlockRequests() is called.
 */

// --- struct block_request

// TODO: if we keep this, add equivalent API to ActiveRequest
void tr_peerMgrClientSentRequests(tr_torrent* torrent, tr_peer* peer, tr_block_span_t span)
{
    auto const now = tr_time();

    for (tr_block_index_t block = span.begin; block < span.end; ++block)
    {
        torrent->swarm->active_requests.add(block, peer, now);
    }
}

std::vector<tr_block_span_t> tr_peerMgrGetNextRequests(tr_torrent* torrent, tr_peer const* peer, size_t numwant)
{
    class MediatorImpl final : public Wishlist::Mediator
    {
    public:
        MediatorImpl(tr_torrent const* torrent_in, tr_peer const* peer_in)
            : torrent_{ torrent_in }
            , swarm_{ torrent_in->swarm }
            , peer_{ peer_in }
        {
        }

        MediatorImpl(MediatorImpl&&) = delete;
        MediatorImpl(MediatorImpl const&) = delete;
        MediatorImpl& operator=(MediatorImpl&&) = delete;
        MediatorImpl& operator=(MediatorImpl const&) = delete;

        ~MediatorImpl() override = default;

        [[nodiscard]] bool clientCanRequestBlock(tr_block_index_t block) const override
        {
            return !torrent_->has_block(block) && !swarm_->active_requests.has(block, peer_);
        }

        [[nodiscard]] bool clientCanRequestPiece(tr_piece_index_t piece) const override
        {
            return torrent_->piece_is_wanted(piece) && peer_->hasPiece(piece);
        }

        [[nodiscard]] bool isEndgame() const override
        {
            return swarm_->isEndgame();
        }

        [[nodiscard]] size_t countActiveRequests(tr_block_index_t block) const override
        {
            return swarm_->active_requests.count(block);
        }

        [[nodiscard]] size_t countMissingBlocks(tr_piece_index_t piece) const override
        {
            return torrent_->count_missing_blocks_in_piece(piece);
        }

        [[nodiscard]] tr_block_span_t blockSpan(tr_piece_index_t piece) const override
        {
            return torrent_->block_span_for_piece(piece);
        }

        [[nodiscard]] tr_piece_index_t countAllPieces() const override
        {
            return torrent_->piece_count();
        }

        [[nodiscard]] tr_priority_t priority(tr_piece_index_t piece) const override
        {
            return torrent_->piece_priority(piece);
        }

        [[nodiscard]] bool isSequentialDownload() const override
        {
            return torrent_->is_sequential_download();
        }

    private:
        tr_torrent const* const torrent_;
        tr_swarm const* const swarm_;
        tr_peer const* const peer_;
    };

    torrent->swarm->updateEndgame();
    auto const mediator = MediatorImpl{ torrent, peer };
    return Wishlist{ mediator }.next(numwant);
}

// --- Piece List Manipulation / Accessors

bool tr_peerMgrDidPeerRequest(tr_torrent const* tor, tr_peer const* peer, tr_block_index_t block)
{
    return tor->swarm->active_requests.has(block, peer);
}

size_t tr_peerMgrCountActiveRequestsToPeer(tr_torrent const* tor, tr_peer const* peer)
{
    return tor->swarm->active_requests.count(peer);
}

void tr_peerMgr::refillUpkeep() const
{
    auto const lock = unique_lock();

    for (auto* const tor : session->torrents())
    {
        tor->swarm->cancelOldRequests();
    }
}

namespace
{
namespace handshake_helpers
{
void create_bit_torrent_peer(tr_torrent* tor, std::shared_ptr<tr_peerIo> io, tr_peer_info* peer_info, tr_interned_string client)
{
    TR_ASSERT(peer_info != nullptr);
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm != nullptr);

    tr_swarm* swarm = tor->swarm;

    auto* peer = tr_peerMsgsNew(tor, peer_info, std::move(io), client, &tr_swarm::peer_callback_bt, swarm);

    swarm->peers.push_back(peer);

    ++swarm->stats.peer_count;
    ++swarm->stats.peer_from_count[peer_info->from_first()];

    TR_ASSERT(swarm->stats.peer_count == swarm->peerCount());
    TR_ASSERT(swarm->stats.peer_from_count[peer_info->from_first()] <= swarm->stats.peer_count);
}

/* FIXME: this is kind of a mess. */
[[nodiscard]] bool on_handshake_done(tr_peerMgr* const manager, tr_handshake::Result const& result)
{
    TR_ASSERT(result.io != nullptr);

    bool const ok = result.is_connected;

    auto* const s = manager->get_existing_swarm(result.io->torrent_hash());

    auto const& socket_address = result.io->socket_address();

    if (result.io->is_incoming())
    {
        manager->incoming_handshakes.erase(socket_address);
    }
    else if (s != nullptr)
    {
        s->outgoing_handshakes.erase(socket_address);
    }

    auto const lock = manager->unique_lock();

    if (!ok || s == nullptr || !s->is_running)
    {
        if (s != nullptr)
        {
            if (auto* const info = s->get_existing_peer_info(socket_address); info != nullptr && !info->is_connected())
            {
                info->on_connection_failed();

                if (!result.read_anything_from_peer)
                {
                    tr_logAddTraceSwarm(
                        s,
                        fmt::format(
                            "marking peer {} as unreachable... num_fails is {}",
                            info->display_name(),
                            info->connection_failure_count()));
                    info->set_connectable(false);
                }
            }
        }
    }
    else /* looking good */
    {
        // If this is an outgoing connection, then we are sure we already have the peer info object
        auto& info = result.io->is_incoming() ? s->ensure_info_exists(socket_address, 0U, TR_PEER_FROM_INCOMING, false) :
                                                *s->get_existing_peer_info(socket_address);

        if (!result.io->is_incoming())
        {
            info.set_connectable();
        }

        // If we're connected via µTP, then we know the peer supports µTP...
        if (result.io->is_utp())
        {
            info.set_utp_supported();
        }

        if (info.is_banned())
        {
            tr_logAddTraceSwarm(s, fmt::format("banned peer {} tried to reconnect", info.display_name()));
        }
        else if (s->peerCount() >= s->tor->peer_limit())
        {
            // too many peers already
        }
        else if (info.is_connected())
        {
            // we're already connected to this peer; do nothing
        }
        else
        {
            auto client = tr_interned_string{};
            if (result.peer_id)
            {
                auto buf = std::array<char, 128>{};
                tr_clientForId(std::data(buf), sizeof(buf), *result.peer_id);
                client = tr_interned_string{ tr_quark_new(std::data(buf)) };
            }

            result.io->set_bandwidth(&s->tor->bandwidth_);
            create_bit_torrent_peer(s->tor, result.io, &info, client);

            return true;
        }
    }

    return false;
}
} // namespace handshake_helpers
} // namespace

void tr_peerMgrAddIncoming(tr_peerMgr* manager, tr_peer_socket&& socket)
{
    using namespace handshake_helpers;

    TR_ASSERT(manager->session != nullptr);
    auto const lock = manager->unique_lock();

    tr_session* session = manager->session;

    if (session->addressIsBlocked(socket.address()))
    {
        tr_logAddTrace(fmt::format("Banned IP address '{}' tried to connect to us", socket.display_name()));
        socket.close();
    }
    else if (manager->incoming_handshakes.count(socket.socket_address()) != 0U)
    {
        socket.close();
    }
    else /* we don't have a connection to them yet... */
    {
        auto socket_address = socket.socket_address();
        manager->incoming_handshakes.try_emplace(
            socket_address,
            &manager->handshake_mediator_,
            tr_peerIo::new_incoming(session, &session->top_bandwidth_, std::move(socket)),
            session->encryptionMode(),
            [manager](tr_handshake::Result const& result) { return on_handshake_done(manager, result); });
    }
}

size_t tr_peerMgrAddPex(tr_torrent* tor, tr_peer_from from, tr_pex const* pex, size_t n_pex)
{
    size_t n_used = 0;
    tr_swarm* s = tor->swarm;
    auto const lock = s->manager->unique_lock();

    for (tr_pex const* const end = pex + n_pex; pex != end; ++pex)
    {
        if (tr_isPex(pex) && /* safeguard against corrupt data */
            !s->manager->session->addressIsBlocked(pex->addr) && pex->is_valid_for_peers() && from != TR_PEER_FROM_INCOMING &&
            (from != TR_PEER_FROM_PEX || (pex->flags & ADDED_F_CONNECTABLE) != 0))
        {
            // we store this peer since it is supposedly connectable (socket address should be the peer's listening address)
            // don't care about non-connectable peers that we are not connected to
            s->ensure_info_exists({ pex->addr, pex->port }, pex->flags, from, true);
            ++n_used;
        }
    }

    return n_used;
}

std::vector<tr_pex> tr_pex::from_compact_ipv4(
    void const* compact,
    size_t compact_len,
    uint8_t const* added_f,
    size_t added_f_len)
{
    size_t const n = compact_len / tr_socket_address::CompactSockAddrBytes[TR_AF_INET];
    auto const* walk = static_cast<std::byte const*>(compact);
    auto pex = std::vector<tr_pex>(n);

    for (size_t i = 0; i < n; ++i)
    {
        std::tie(pex[i].addr, walk) = tr_address::from_compact_ipv4(walk);
        std::tie(pex[i].port, walk) = tr_port::fromCompact(walk);

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    return pex;
}

std::vector<tr_pex> tr_pex::from_compact_ipv6(
    void const* compact,
    size_t compact_len,
    uint8_t const* added_f,
    size_t added_f_len)
{
    size_t const n = compact_len / tr_socket_address::CompactSockAddrBytes[TR_AF_INET6];
    auto const* walk = static_cast<std::byte const*>(compact);
    auto pex = std::vector<tr_pex>(n);

    for (size_t i = 0; i < n; ++i)
    {
        std::tie(pex[i].addr, walk) = tr_address::from_compact_ipv6(walk);
        std::tie(pex[i].port, walk) = tr_port::fromCompact(walk);

        if (added_f != nullptr && n == added_f_len)
        {
            pex[i].flags = added_f[i];
        }
    }

    return pex;
}

// ---

namespace
{
namespace get_peers_helpers
{

[[nodiscard]] bool is_peer_interesting(tr_torrent const* tor, tr_peer_info const& info)
{
    if (tor->is_done() && info.is_seed())
    {
        return false;
    }

    if (tor->swarm->peer_is_in_use(info))
    {
        return true;
    }

    if (info.is_blocklisted(tor->session))
    {
        return false;
    }

    if (info.is_banned())
    {
        return false;
    }

    return true;
}

} // namespace get_peers_helpers
} // namespace

std::vector<tr_pex> tr_peerMgrGetPeers(tr_torrent const* tor, uint8_t address_type, uint8_t list_mode, size_t max_peer_count)
{
    using namespace get_peers_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();

    TR_ASSERT(address_type == TR_AF_INET || address_type == TR_AF_INET6);
    TR_ASSERT(list_mode == TR_PEERS_CONNECTED || list_mode == TR_PEERS_INTERESTING);

    tr_swarm const* s = tor->swarm;

    // build a list of peer info objects

    auto infos = std::vector<tr_peer_info const*>{};
    if (list_mode == TR_PEERS_CONNECTED) /* connected peers only */
    {
        auto const& peers = s->peers;
        infos.reserve(std::size(peers));
        for (auto const* peer : peers)
        {
            if (peer->socket_address().address().type == address_type)
            {
                infos.emplace_back(peer->peer_info);
            }
        }
    }
    else /* TR_PEERS_INTERESTING */
    {
        auto const& pool = s->connectable_pool;
        infos.reserve(std::size(pool));
        for (auto const& [socket_address, peer_info] : pool)
        {
            TR_ASSERT(socket_address == peer_info.listen_socket_address());
            if (socket_address.address().type == address_type && is_peer_interesting(tor, peer_info))
            {
                infos.emplace_back(&peer_info);
            }
        }
    }

    // add the N most useful peers into our return list

    auto const n = std::min(std::size(infos), max_peer_count);
    auto pex = std::vector<tr_pex>{};
    pex.reserve(n);

    std::partial_sort(std::begin(infos), std::begin(infos) + n, std::end(infos), CompareAtomsByUsefulness);
    infos.resize(n);

    for (auto const* const info : infos)
    {
        auto const& [addr, port] = info->listen_socket_address();

        TR_ASSERT(addr.is_valid());
        TR_ASSERT(addr.type == address_type);
        pex.emplace_back(addr, port, info->pex_flags());
    }

    std::sort(std::begin(pex), std::end(pex));
    return pex;
}

void tr_swarm::on_torrent_started()
{
    auto const lock = tor->unique_lock();
    is_running = true;
    manager->rechokeSoon();
}

void tr_swarm::on_torrent_stopped()
{
    stop();
}

void tr_peerMgrAddTorrent(tr_peerMgr* manager, tr_torrent* tor)
{
    TR_ASSERT(tr_isTorrent(tor));
    auto const lock = tor->unique_lock();
    TR_ASSERT(tor->swarm == nullptr);

    tor->swarm = new tr_swarm{ manager, tor };
}

int8_t tr_peerMgrPieceAvailability(tr_torrent const* tor, tr_piece_index_t piece)
{
    if (!tor->has_metainfo())
    {
        return 0;
    }

    if (tor->is_seed() || tor->has_piece(piece))
    {
        return -1;
    }

    auto const& peers = tor->swarm->peers;
    return std::count_if(std::begin(peers), std::end(peers), [piece](auto const* peer) { return peer->hasPiece(piece); });
}

void tr_peerMgrTorrentAvailability(tr_torrent const* tor, int8_t* tab, unsigned int n_tabs)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tab != nullptr);
    TR_ASSERT(n_tabs > 0);

    std::fill_n(tab, n_tabs, int8_t{});

    auto const interval = tor->piece_count() / static_cast<float>(n_tabs);
    for (tr_piece_index_t i = 0; i < n_tabs; ++i)
    {
        auto const piece = static_cast<tr_piece_index_t>(i * interval);
        tab[i] = tr_peerMgrPieceAvailability(tor, piece);
    }
}

tr_swarm_stats tr_swarmGetStats(tr_swarm const* const swarm)
{
    TR_ASSERT(swarm != nullptr);

    auto count_active_peers = [&swarm](tr_direction dir)
    {
        return std::count_if(
            std::begin(swarm->peers),
            std::end(swarm->peers),
            [dir](auto const& peer) { return peer->is_active(dir); });
    };

    auto& stats = swarm->stats;
    stats.active_peer_count[TR_UP] = count_active_peers(TR_UP);
    stats.active_peer_count[TR_DOWN] = count_active_peers(TR_DOWN);
    stats.active_webseed_count = swarm->countActiveWebseeds(tr_time_msec());
    return stats;
}

/* count how many bytes we want that connected peers have */
uint64_t tr_peerMgrGetDesiredAvailable(tr_torrent const* tor)
{
    TR_ASSERT(tr_isTorrent(tor));

    // common shortcuts...

    if (!tor->is_running() || tor->is_stopping() || tor->is_done() || !tor->has_metainfo())
    {
        return 0;
    }

    tr_swarm const* const swarm = tor->swarm;
    if (swarm == nullptr || std::empty(swarm->peers))
    {
        return 0;
    }

    auto available = swarm->peers.front()->has();
    for (auto const* const peer : swarm->peers)
    {
        available |= peer->has();
    }

    if (available.has_all())
    {
        return tor->left_until_done();
    }

    auto desired_available = uint64_t{};

    for (tr_piece_index_t i = 0, n = tor->piece_count(); i < n; ++i)
    {
        if (tor->piece_is_wanted(i) && available.test(i))
        {
            desired_available += tor->count_missing_bytes_in_piece(i);
        }
    }

    TR_ASSERT(desired_available <= tor->total_size());
    return desired_available;
}

tr_webseed_view tr_peerMgrWebseed(tr_torrent const* tor, size_t i)
{
    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm != nullptr);
    size_t const n = std::size(tor->swarm->webseeds);
    TR_ASSERT(i < n);

    return i >= n ? tr_webseed_view{} : tr_webseedView(tor->swarm->webseeds[i].get());
}

namespace
{
namespace peer_stat_helpers
{

[[nodiscard]] auto get_peer_stats(tr_peerMsgs const* peer, time_t now, uint64_t now_msec)
{
    auto stats = tr_peer_stat{};

    auto const [addr, port] = peer->socket_address();

    addr.display_name(stats.addr, sizeof(stats.addr));
    stats.client = peer->user_agent().c_str();
    stats.port = port.host();
    stats.from = peer->peer_info->from_first();
    stats.progress = peer->percentDone();
    stats.isUTP = peer->is_utp_connection();
    stats.isEncrypted = peer->is_encrypted();
    stats.rateToPeer_KBps = tr_toSpeedKBps(peer->get_piece_speed_bytes_per_second(now_msec, TR_CLIENT_TO_PEER));
    stats.rateToClient_KBps = tr_toSpeedKBps(peer->get_piece_speed_bytes_per_second(now_msec, TR_PEER_TO_CLIENT));
    stats.peerIsChoked = peer->peer_is_choked();
    stats.peerIsInterested = peer->peer_is_interested();
    stats.clientIsChoked = peer->client_is_choked();
    stats.clientIsInterested = peer->client_is_interested();
    stats.isIncoming = peer->is_incoming_connection();
    stats.isDownloadingFrom = peer->is_active(TR_PEER_TO_CLIENT);
    stats.isUploadingTo = peer->is_active(TR_CLIENT_TO_PEER);
    stats.isSeed = peer->isSeed();

    stats.blocksToPeer = peer->blocks_sent_to_peer.count(now, CancelHistorySec);
    stats.blocksToClient = peer->blocks_sent_to_client.count(now, CancelHistorySec);
    stats.cancelsToPeer = peer->cancels_sent_to_peer.count(now, CancelHistorySec);
    stats.cancelsToClient = peer->cancels_sent_to_client.count(now, CancelHistorySec);

    stats.activeReqsToPeer = peer->activeReqCount(TR_CLIENT_TO_PEER);
    stats.activeReqsToClient = peer->activeReqCount(TR_PEER_TO_CLIENT);

    char* pch = stats.flagStr;

    if (stats.isUTP)
    {
        *pch++ = 'T';
    }

    if (peer->swarm->optimistic == peer)
    {
        *pch++ = 'O';
    }

    if (stats.isDownloadingFrom)
    {
        *pch++ = 'D';
    }
    else if (stats.clientIsInterested)
    {
        *pch++ = 'd';
    }

    if (stats.isUploadingTo)
    {
        *pch++ = 'U';
    }
    else if (stats.peerIsInterested)
    {
        *pch++ = 'u';
    }

    if (!stats.clientIsChoked && !stats.clientIsInterested)
    {
        *pch++ = 'K';
    }

    if (!stats.peerIsChoked && !stats.peerIsInterested)
    {
        *pch++ = '?';
    }

    if (stats.isEncrypted)
    {
        *pch++ = 'E';
    }

    if (stats.from == TR_PEER_FROM_DHT)
    {
        *pch++ = 'H';
    }
    else if (stats.from == TR_PEER_FROM_PEX)
    {
        *pch++ = 'X';
    }

    if (stats.isIncoming)
    {
        *pch++ = 'I';
    }

    *pch = '\0';

    return stats;
}

} // namespace peer_stat_helpers
} // namespace

tr_peer_stat* tr_peerMgrPeerStats(tr_torrent const* tor, size_t* setme_count)
{
    using namespace peer_stat_helpers;

    TR_ASSERT(tr_isTorrent(tor));
    TR_ASSERT(tor->swarm->manager != nullptr);

    auto const peers = tor->swarm->peers;
    auto const n = std::size(peers);
    auto* const ret = new tr_peer_stat[n];

    auto const now = tr_time();
    auto const now_msec = tr_time_msec();
    std::transform(
        std::begin(peers),
        std::end(peers),
        ret,
        [&now, &now_msec](auto const* peer) { return get_peer_stats(peer, now, now_msec); });

    *setme_count = n;
    return ret;
}

namespace
{
namespace update_interest_helpers
{
/* does this peer have any pieces that we want? */
[[nodiscard]] bool isPeerInteresting(
    tr_torrent const* const tor,
    std::vector<bool> const& piece_is_interesting,
    tr_peerMsgs const* const peer)
{
    /* these cases should have already been handled by the calling code... */
    TR_ASSERT(!tor->is_done());
    TR_ASSERT(tor->client_can_download());

    if (peer->isSeed())
    {
        return true;
    }

    for (tr_piece_index_t i = 0; i < tor->piece_count(); ++i)
    {
        if (piece_is_interesting[i] && peer->hasPiece(i))
        {
            return true;
        }
    }

    return false;
}

// determine which peers to show interest in
void updateInterest(tr_swarm* swarm)
{
    // sometimes this function isn't necessary
    auto const* const tor = swarm->tor;
    if (tor->is_done() || !tor->client_can_download())
    {
        return;
    }

    if (auto const& peers = swarm->peers; !std::empty(peers))
    {
        int const n = tor->piece_count();

        // build a bitfield of interesting pieces...
        auto piece_is_interesting = std::vector<bool>(n);
        for (int i = 0; i < n; ++i)
        {
            piece_is_interesting[i] = tor->piece_is_wanted(i) && !tor->has_piece(i);
        }

        for (auto* const peer : peers)
        {
            peer->set_interested(isPeerInteresting(tor, piece_is_interesting, peer));
        }
    }
}
} // namespace update_interest_helpers
} // namespace

// ---

namespace
{
namespace rechoke_uploads_helpers
{
struct ChokeData
{
    ChokeData(tr_peerMsgs* msgs_in, int rate_in, uint8_t salt_in, bool is_interested_in, bool was_choked_in, bool is_choked_in)
        : msgs{ msgs_in }
        , rate{ rate_in }
        , salt{ salt_in }
        , is_interested{ is_interested_in }
        , was_choked{ was_choked_in }
        , is_choked{ is_choked_in }
    {
    }

    tr_peerMsgs* msgs;
    int rate;
    uint8_t salt;
    bool is_interested;
    bool was_choked;
    bool is_choked;

    [[nodiscard]] constexpr auto compare(ChokeData const& that) const noexcept // <=>
    {
        // prefer higher overall speeds
        if (auto const val = tr_compare_3way(this->rate, that.rate); val != 0)
        {
            return -val;
        }

        if (this->was_choked != that.was_choked) // prefer unchoked
        {
            return this->was_choked ? 1 : -1;
        }

        return tr_compare_3way(this->salt, that.salt);
    }

    [[nodiscard]] constexpr auto operator<(ChokeData const& that) const noexcept
    {
        return compare(that) < 0;
    }
};

/* get a rate for deciding which peers to choke and unchoke. */
[[nodiscard]] auto getRateBps(tr_torrent const* tor, tr_peer const* peer, uint64_t now)
{
    if (tor->is_done())
    {
        return peer->get_piece_speed_bytes_per_second(now, TR_CLIENT_TO_PEER);
    }

    /* downloading a private torrent... take upload speed into account
     * because there may only be a small window of opportunity to share */
    if (tor->is_private())
    {
        return peer->get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT) +
            peer->get_piece_speed_bytes_per_second(now, TR_CLIENT_TO_PEER);
    }

    /* downloading a public torrent */
    return peer->get_piece_speed_bytes_per_second(now, TR_PEER_TO_CLIENT);
}

// an optimistically unchoked peer is immune from rechoking
// for this many calls to rechokeUploads().
auto constexpr OptimisticUnchokeMultiplier = uint8_t{ 4 };

void rechokeUploads(tr_swarm* s, uint64_t const now)
{
    auto const lock = s->unique_lock();

    auto& peers = s->peers;
    auto const peer_count = std::size(peers);
    auto choked = std::vector<ChokeData>{};
    choked.reserve(peer_count);
    auto const* const session = s->manager->session;
    bool const choke_all = !s->tor->client_can_upload();
    bool const is_maxed_out = s->tor->bandwidth_.is_maxed_out(TR_UP, now);

    /* an optimistic unchoke peer's "optimistic"
     * state lasts for N calls to rechokeUploads(). */
    if (s->optimistic_unchoke_time_scaler > 0)
    {
        --s->optimistic_unchoke_time_scaler;
    }
    else
    {
        s->optimistic = nullptr;
    }

    /* sort the peers by preference and rate */
    auto salter = tr_salt_shaker{};
    for (auto* const peer : peers)
    {
        if (peer->isSeed())
        {
            /* choke seeds and partial seeds */
            peer->set_choke(true);
        }
        else if (choke_all)
        {
            /* choke everyone if we're not uploading */
            peer->set_choke(true);
        }
        else if (peer != s->optimistic)
        {
            choked.emplace_back(
                peer,
                getRateBps(s->tor, peer, now),
                salter(),
                peer->peer_is_interested(),
                peer->peer_is_choked(),
                true);
        }
    }

    std::sort(std::begin(choked), std::end(choked));

    /**
     * Reciprocation and number of uploads capping is managed by unchoking
     * the N peers which have the best upload rate and are interested.
     * This maximizes the client's download rate. These N peers are
     * referred to as downloaders, because they are interested in downloading
     * from the client.
     *
     * Peers which have a better upload rate (as compared to the downloaders)
     * but aren't interested get unchoked. If they become interested, the
     * downloader with the worst upload rate gets choked. If a client has
     * a complete file, it uses its upload rate rather than its download
     * rate to decide which peers to unchoke.
     *
     * If our bandwidth is maxed out, don't unchoke any more peers.
     */
    auto checked_choke_count = size_t{ 0U };
    auto unchoked_interested = size_t{ 0U };

    for (auto& item : choked)
    {
        if (unchoked_interested >= session->uploadSlotsPerTorrent())
        {
            break;
        }

        item.is_choked = is_maxed_out ? item.was_choked : false;

        ++checked_choke_count;

        if (item.is_interested)
        {
            ++unchoked_interested;
        }
    }

    /* optimistic unchoke */
    if (s->optimistic == nullptr && !is_maxed_out && checked_choke_count < std::size(choked))
    {
        auto rand_pool = std::vector<ChokeData*>{};

        for (auto i = checked_choke_count, n = std::size(choked); i < n; ++i)
        {
            if (choked[i].is_interested)
            {
                rand_pool.push_back(&choked[i]);
            }
        }

        if (auto const n = std::size(rand_pool); n != 0)
        {
            auto* c = rand_pool[tr_rand_int(n)];
            c->is_choked = false;
            s->optimistic = c->msgs;
            s->optimistic_unchoke_time_scaler = OptimisticUnchokeMultiplier;
        }
    }

    for (auto& item : choked)
    {
        item.msgs->set_choke(item.is_choked);
    }
}
} // namespace rechoke_uploads_helpers
} // namespace

void tr_peerMgr::rechokePulse() const
{
    using namespace update_interest_helpers;
    using namespace rechoke_uploads_helpers;

    auto const lock = unique_lock();
    auto const now = tr_time_msec();

    for (auto* const tor : session->torrents())
    {
        if (tor->is_running())
        {
            // possibly stop torrents that have seeded enough
            tr_torrentCheckSeedLimit(tor);
        }

        if (tor->is_running())
        {
            if (auto* const swarm = tor->swarm; swarm->stats.peer_count > 0)
            {
                rechokeUploads(swarm, now);
                updateInterest(swarm);
            }
        }
    }
}

// --- Life and Death

namespace
{
namespace disconnect_helpers
{
// when many peers are available, keep idle ones this long
auto constexpr MinUploadIdleSecs = time_t{ 60 };

// when few peers are available, keep idle ones this long
auto constexpr MaxUploadIdleSecs = time_t{ 60 * 5 };

[[nodiscard]] bool shouldPeerBeClosed(tr_swarm const* s, tr_peerMsgs const* peer, size_t peer_count, time_t const now)
{
    /* if it's marked for purging, close it */
    if (peer->do_purge)
    {
        tr_logAddTraceSwarm(s, fmt::format("purging peer {} because its do_purge flag is set", peer->display_name()));
        return true;
    }

    auto const* tor = s->tor;
    auto const* const info = peer->peer_info;

    /* disconnect if we're both seeds and enough time has passed for PEX */
    if (tor->is_done() && peer->isSeed())
    {
        return !tor->allows_pex() || info->idle_secs(now).value_or(0U) >= 30U;
    }

    /* disconnect if it's been too long since piece data has been transferred.
     * this is on a sliding scale based on number of available peers... */
    {
        auto const relax_strictness_if_fewer_than_n = static_cast<size_t>(std::lround(tor->peer_limit() * 0.9));
        /* if we have >= relaxIfFewerThan, strictness is 100%.
         * if we have zero connections, strictness is 0% */
        float const strictness = peer_count >= relax_strictness_if_fewer_than_n ?
            1.0 :
            peer_count / (float)relax_strictness_if_fewer_than_n;
        auto const lo = MinUploadIdleSecs;
        auto const hi = MaxUploadIdleSecs;
        time_t const limit = hi - (hi - lo) * strictness;

        if (auto const idle_secs = info->idle_secs(now); idle_secs && *idle_secs > limit)
        {
            tr_logAddTraceSwarm(
                s,
                fmt::format(
                    "purging peer {} because it's been {} secs since we shared anything",
                    peer->display_name(),
                    *idle_secs));
            return true;
        }
    }

    return false;
}

void close_peer(tr_peerMsgs* peer)
{
    TR_ASSERT(peer != nullptr);
    peer->swarm->remove_peer(peer);
}

constexpr struct
{
    [[nodiscard]] constexpr static int compare(tr_peerMsgs const* a, tr_peerMsgs const* b) // <=>
    {
        if (a->do_purge != b->do_purge)
        {
            return a->do_purge ? 1 : -1;
        }

        return -a->peer_info->compare_by_piece_data_time(*b->peer_info);
    }

    [[nodiscard]] constexpr bool operator()(tr_peerMsgs const* a, tr_peerMsgs const* b) const // less than
    {
        return compare(a, b) < 0;
    }
} ComparePeerByMostActive{};

constexpr auto ComparePeerByLeastActive = [](tr_peerMsgs const* a, tr_peerMsgs const* b)
{
    return ComparePeerByMostActive(b, a);
};

using bad_peers_t = small::vector<tr_peerMsgs*, 512U>;

bad_peers_t& get_peers_to_close(tr_swarm const* const swarm, time_t const now_sec, bad_peers_t& bad_peers_buf)
{
    auto const& peers = swarm->peers;
    auto const peer_count = std::size(peers);

    bad_peers_buf.clear();
    bad_peers_buf.reserve(peer_count);
    for (auto* peer : swarm->peers)
    {
        if (shouldPeerBeClosed(swarm, peer, peer_count, now_sec))
        {
            bad_peers_buf.emplace_back(peer);
        }
    }

    return bad_peers_buf;
}

void close_bad_peers(tr_swarm* s, time_t const now_sec, bad_peers_t& bad_peers_buf)
{
    for (auto* peer : get_peers_to_close(s, now_sec, bad_peers_buf))
    {
        tr_logAddTraceSwarm(peer->swarm, fmt::format("removing bad peer {}", peer->display_name()));
        close_peer(peer);
    }
}

void enforceSwarmPeerLimit(tr_swarm* swarm, size_t max)
{
    // do we have too many peers?
    auto const n = swarm->peerCount();
    if (n <= max)
    {
        return;
    }

    // close all but the `max` most active
    auto peers = std::vector<tr_peerMsgs*>(n - max);
    std::partial_sort_copy(
        std::begin(swarm->peers),
        std::end(swarm->peers),
        std::begin(peers),
        std::end(peers),
        ComparePeerByLeastActive);
    std::for_each(std::begin(peers), std::end(peers), close_peer);
}

void enforceSessionPeerLimit(tr_session* session)
{
    // No need to disconnect if we are under the peer limit
    auto const max = session->peerLimit();
    if (tr_peerMsgs::size() <= max)
    {
        return;
    }

    // Make a list of all the peers.
    auto peers = std::vector<tr_peerMsgs*>{};
    peers.reserve(tr_peerMsgs::size());
    for (auto const* const tor : session->torrents())
    {
        peers.insert(std::end(peers), std::begin(tor->swarm->peers), std::end(tor->swarm->peers));
    }

    TR_ASSERT(tr_peerMsgs::size() == std::size(peers));
    if (std::size(peers) > max)
    {
        std::partial_sort(std::begin(peers), std::begin(peers) + max, std::end(peers), ComparePeerByMostActive);
        std::for_each(std::begin(peers) + max, std::end(peers), close_peer);
    }
}
} // namespace disconnect_helpers
} // namespace

void tr_peerMgr::reconnectPulse()
{
    using namespace disconnect_helpers;

    auto const lock = session->unique_lock();
    auto const now_sec = tr_time();

    // remove crappy peers
    auto bad_peers_buf = bad_peers_t{};
    for (auto* const tor : session->torrents())
    {
        auto* const swarm = tor->swarm;

        if (!swarm->is_running)
        {
            swarm->remove_all_peers();
        }
        else
        {
            close_bad_peers(swarm, now_sec, bad_peers_buf);
        }
    }

    // if we're over the per-torrent peer limits, cull some peers
    for (auto* const tor : session->torrents())
    {
        if (tor->is_running())
        {
            enforceSwarmPeerLimit(tor->swarm, tor->peer_limit());
        }
    }

    // if we're over the per-session peer limits, cull some peers
    enforceSessionPeerLimit(session);

    // try to make new peer connections
    make_new_peer_connections();
}

// --- Bandwidth Allocation

namespace
{
namespace bandwidth_helpers
{

void pumpAllPeers(tr_peerMgr* mgr)
{
    for (auto* const tor : mgr->session->torrents())
    {
        for (auto* const peer : tor->swarm->peers)
        {
            peer->pulse();
        }
    }
}

void queuePulse(tr_session* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(tr_isDirection(dir));

    if (!session->queueEnabled(dir))
    {
        return;
    }

    auto const n = session->countQueueFreeSlots(dir);
    for (auto* tor : session->getNextQueuedTorrents(dir, n))
    {
        tr_torrentStartNow(tor);
        session->onQueuedTorrentStarted(tor);
    }
}

} // namespace bandwidth_helpers
} // namespace

void tr_peerMgr::bandwidthPulse()
{
    using namespace bandwidth_helpers;

    auto const lock = unique_lock();

    pumpAllPeers(this);

    // allocate bandwidth to the peers
    static auto constexpr Msec = std::chrono::duration_cast<std::chrono::milliseconds>(BandwidthTimerPeriod).count();
    session->top_bandwidth_.allocate(Msec);

    // torrent upkeep
    for (auto* const tor : session->torrents())
    {
        tor->do_idle_work();
        tr_torrentMagnetDoIdleWork(tor);
    }

    /* pump the queues */
    queuePulse(session, TR_UP);
    queuePulse(session, TR_DOWN);

    reconnectPulse();
}

// ---

bool tr_swarm::peer_is_in_use(tr_peer_info const& peer_info) const
{
    // TODO(tearfur): maybe it's possible to store each handshake in the peer_info objects
    return peer_info.is_connected() || outgoing_handshakes.count(peer_info.listen_socket_address()) != 0U;
}

namespace
{
namespace connect_helpers
{
/* is this atom someone that we'd want to initiate a connection to? */
[[nodiscard]] bool is_peer_candidate(tr_torrent const* tor, tr_peer_info const& peer_info, time_t const now)
{
    // have we already tried and failed to connect?
    if (auto const conn = peer_info.is_connectable(); conn && !*conn)
    {
        return false;
    }

    // not if we're both seeds
    if (tor->is_done() && peer_info.is_seed())
    {
        return false;
    }

    // not if we've already got a connection to them...
    if (tor->swarm->peer_is_in_use(peer_info))
    {
        return false;
    }

    // not if we just tried them already
    if (!peer_info.reconnect_interval_has_passed(now))
    {
        return false;
    }

    // not if they're blocklisted
    if (peer_info.is_blocklisted(tor->session))
    {
        return false;
    }

    // not if they're banned...
    if (peer_info.is_banned())
    {
        return false;
    }

    return true;
}

struct peer_candidate
{
    peer_candidate() = default;

    peer_candidate(uint64_t score_in, tr_torrent const* const tor_in, tr_peer_info const* const peer_info_in)
        : score{ score_in }
        , tor{ tor_in }
        , peer_info{ peer_info_in }
    {
    }

    uint64_t score;
    tr_torrent const* tor;
    tr_peer_info const* peer_info;
};

[[nodiscard]] bool torrentWasRecentlyStarted(tr_torrent const* tor)
{
    return difftime(tr_time(), tor->startDate) < 120;
}

[[nodiscard]] constexpr uint64_t addValToKey(uint64_t value, int width, uint64_t addme)
{
    value = value << (uint64_t)width;
    value |= addme;
    return value;
}

/* smaller value is better */
[[nodiscard]] uint64_t getPeerCandidateScore(tr_torrent const* tor, tr_peer_info const& peer_info, uint8_t salt)
{
    auto i = uint64_t{};
    auto score = uint64_t{};

    /* prefer peers we've connected to, or never tried, over peers we failed to connect to. */
    i = peer_info.connection_failure_count() != 0U ? 1U : 0U;
    score = addValToKey(score, 1, i);

    /* prefer the one we attempted least recently (to cycle through all peers) */
    i = peer_info.connection_attempt_time();
    score = addValToKey(score, 32, i);

    /* prefer peers belonging to a torrent of a higher priority */
    switch (tor->get_priority())
    {
    case TR_PRI_HIGH:
        i = 0;
        break;

    case TR_PRI_NORMAL:
        i = 1;
        break;

    case TR_PRI_LOW:
        i = 2;
        break;
    }

    score = addValToKey(score, 4, i);

    /* prefer recently-started torrents */
    i = torrentWasRecentlyStarted(tor) ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* prefer torrents we're downloading with */
    i = tor->is_done() ? 1 : 0;
    score = addValToKey(score, 1, i);

    /* prefer peers that are known to be connectible */
    i = peer_info.is_connectable().value_or(false) ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* prefer peers that we might be able to upload to */
    i = peer_info.is_seed() ? 0 : 1;
    score = addValToKey(score, 1, i);

    /* Prefer peers that we got from more trusted sources.
     * lower `fromBest` values indicate more trusted sources */
    score = addValToKey(score, 4, peer_info.from_best());

    /* salt */
    score = addValToKey(score, 8, salt);

    return score;
}

[[nodiscard]] tr_peerMgr::OutboundCandidates get_peer_candidates(tr_session* session)
{
    auto const now = tr_time();
    auto const now_msec = tr_time_msec();

    // leave 5% of connection slots for incoming connections -- ticket #2609
    if (auto const max_candidates = static_cast<size_t>(session->peerLimit() * 0.95); max_candidates <= tr_peerMsgs::size())
    {
        return {};
    }

    auto candidates = std::vector<peer_candidate>{};
    candidates.reserve(tr_peer_info::known_connectable_count());

    /* populate the candidate array */
    auto salter = tr_salt_shaker{};
    for (auto* const tor : session->torrents())
    {
        auto* const swarm = tor->swarm;

        if (!swarm->is_running)
        {
            continue;
        }

        /* if everyone in the swarm is seeds and pex is disabled,
         * then don't initiate connections */
        bool const seeding = tor->is_done();
        if (seeding && swarm->is_all_seeds() && !tor->allows_pex())
        {
            continue;
        }

        /* if we've already got enough peers in this torrent... */
        if (tor->peer_limit() <= swarm->peerCount())
        {
            continue;
        }

        /* if we've already got enough speed in this torrent... */
        if (seeding && tor->bandwidth_.is_maxed_out(TR_UP, now_msec))
        {
            continue;
        }

        for (auto const& [socket_address, atom] : swarm->connectable_pool)
        {
            if (is_peer_candidate(tor, atom, now))
            {
                candidates.emplace_back(getPeerCandidateScore(tor, atom, salter()), tor, &atom);
            }
        }
    }

    // only keep the best `max` candidates
    if (auto const max = tr_peerMgr::OutboundCandidates::requested_inline_size; max < std::size(candidates))
    {
        std::partial_sort(
            std::begin(candidates),
            std::begin(candidates) + max,
            std::end(candidates),
            [](auto const& a, auto const& b) { return a.score < b.score; });
        candidates.resize(max);
    }

    // put the best candiates at the end of the list
    auto ret = tr_peerMgr::OutboundCandidates{};
    for (auto it = std::crbegin(candidates), end = std::crend(candidates); it != end; ++it)
    {
        ret.emplace_back(it->tor->id(), it->peer_info->listen_socket_address());
    }
    return ret;
}

void initiate_connection(tr_peerMgr* mgr, tr_swarm* s, tr_peer_info& peer_info)
{
    using namespace handshake_helpers;

    auto const now = tr_time();
    auto const utp = mgr->session->allowsUTP() && peer_info.supports_utp().value_or(true);
    auto* const session = mgr->session;

    if (tr_peer_socket::limit_reached(session) || (!utp && !session->allowsTCP()))
    {
        return;
    }

    tr_logAddTraceSwarm(
        s,
        fmt::format("Starting an OUTGOING {} connection with {}", utp ? " µTP" : "TCP", peer_info.display_name()));

    auto peer_io = tr_peerIo::new_outgoing(
        session,
        &session->top_bandwidth_,
        peer_info.listen_socket_address(),
        s->tor->info_hash(),
        s->tor->is_seed(),
        utp);

    if (!peer_io)
    {
        tr_logAddTraceSwarm(s, fmt::format("peerIo not created; marking peer {} as unreachable", peer_info.display_name()));
        peer_info.set_connectable(false);
        peer_info.on_connection_failed();
    }
    else
    {
        s->outgoing_handshakes.try_emplace(
            peer_info.listen_socket_address(),
            &mgr->handshake_mediator_,
            peer_io,
            session->encryptionMode(),
            [mgr](tr_handshake::Result const& result) { return on_handshake_done(mgr, result); });
    }

    peer_info.set_connection_attempt_time(now);
}
} // namespace connect_helpers
} // namespace

void tr_peerMgr::make_new_peer_connections()
{
    using namespace connect_helpers;

    auto const lock = session->unique_lock();

    // get the candidates if we need to
    auto& candidates = outbound_candidates_;
    if (std::empty(candidates))
    {
        candidates = get_peer_candidates(session);
    }

    // initiate connections to the last N candidates
    auto const n_this_pass = std::min(std::size(candidates), MaxConnectionsPerPulse);
    auto const it_end = std::crbegin(candidates) + n_this_pass;
    for (auto it = std::crbegin(candidates); it != it_end; ++it)
    {
        auto const& [tor_id, sock_addr] = *it;

        if (auto* const tor = session->torrents().get(tor_id); tor != nullptr)
        {
            if (auto* const peer_info = tor->swarm->get_existing_peer_info(sock_addr); peer_info != nullptr)
            {
                initiate_connection(this, tor->swarm, *peer_info);
            }
        }
    }

    // remove the N candidates that we just consumed
    candidates.resize(std::size(candidates) - n_this_pass);
}

void HandshakeMediator::set_utp_failed(tr_sha1_digest_t const& info_hash, tr_socket_address const& socket_address)
{
    if (auto* const tor = session_.torrents().get(info_hash); tor != nullptr)
    {
        if (auto* const peer_info = tor->swarm->get_existing_peer_info(socket_address); peer_info != nullptr)
        {
            peer_info->set_utp_supported(false);
        }
    }
}
