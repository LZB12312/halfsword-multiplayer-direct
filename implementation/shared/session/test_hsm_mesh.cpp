// Mesh pure parts (session/hsm_mesh.h).
#include "session/hsm_mesh.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    MeshHearing h; uint16_t m = 0xAA;
    CHECK(h.Mask(0) == 0 && !h.Heard(1, 0));
    CHECK(!h.TakeChange(5000, &m));                          // nothing heard, nothing sent: no report
    h.Note(2, 10000); h.Note(3, 10100);
    CHECK(h.Heard(2, 11000) && h.Mask(11000) == 0x0C);
    CHECK(h.Heard(2, 12000) && !h.Heard(2, 12001));          // held 2000 ms
    CHECK(h.TakeChange(11000, &m) && m == 0x0C);
    CHECK(!h.TakeChange(11500, &m));                         // unchanged
    CHECK(!h.TakeChange(11900, &m));                         // still unchanged
    CHECK(h.TakeChange(12050, &m) && m == 0x08);             // slot 2 lapsed (gap since 11000 passed); slot 3 still held
    h.Note(2, 12100); CHECK(!h.TakeChange(12500, &m));       // changed back, but only 450 ms since the last report
    CHECK(h.TakeChange(13100, &m) && m == 0x04);             // slot 3 lapsed at 12100; slot 2 heard again
    CHECK(h.TakeChange(20000, &m) && m == 0);                // everything lapsed
    h.Note(9, 1); CHECK(h.Mask(1) == 0);                     // out of range ignored
    h.Note(1, 30000); CHECK(h.TakeChange(30000, &m) && m == 0x02);
    h.Invalidate(); CHECK(h.TakeChange(30200, &m) && m == 0x02);   // a new host table: the same mask goes out again
    h.Invalidate(); h.Note(1, 99999); CHECK(h.TakeChange(99999, &m) && m == 0x02);
    h.Clear(); CHECK(h.Mask(20000) == 0);

    MeshMasks k;
    k.Set(1, 0xFF);                                          // own bit stripped
    CHECK(!k.Hears(1, 1) && k.Hears(1, 0) && k.Hears(1, 5) && k.Hears(1, 7) && !k.Hears(1, 8));
    k.Set(9, 0xFF); CHECK(!k.Hears(9, 0));                 // no slot 9
    k.Set(8, 0x3FF); CHECK(k.Hears(8, 0) && k.Hears(8, 7) && !k.Hears(8, 8));   // slot 8 (a server's 8th client); bit 9 stripped
    k.Set(7, 0xFF); CHECK(k.Hears(7, 0) && !k.Hears(7, 7));
    std::vector<uint8_t> conn = {0, 1, 2, 3};
    k.Clear(); k.Set(2, 0x02);                               // slot 2 hears slot 1 directly
    auto t = RelayTargets(1, conn, k);
    CHECK(t.size() == 1 && t[0] == 3);                       // 2 skipped, host and sender never
    t = RelayTargets(3, conn, k); CHECK(t.size() == 2);      // slot 2 does not hear 3: relayed
    CHECK(RelayTargets(1, conn) .size() == 2);               // the plain overload is unchanged
    k.Reset(1);                                              // slot 1 left: nobody hears it
    t = RelayTargets(1, conn, k); CHECK(t.size() == 2);
    k.Set(2, 0x08); k.Reset(2); CHECK(!k.Hears(2, 3));
    MeshMasks z; CHECK(RelayTargets(1, conn, z) == RelayTargets(1, conn));   // mask 0 == 0.6.5 behaviour

    // MakePeerMap: host out, bad entries out, slot order.
    auto pe = [](uint8_t slot, uint64_t id) { PeerMapEntry e; e.slot = slot; e.steamId = id; return e; };
    PeerMapMsg pm = MakePeerMap({pe(3, 33), pe(0, 10), pe(1, 11), pe(2, 0), pe(1, 99), pe(4, 11), pe(9, 77), pe(5, 55)});
    CHECK(pm.peers.size() == 3 && pm.peers[0].slot == 1 && pm.peers[1].slot == 3 && pm.peers[2].slot == 5);   // host, no id, dup slot, dup id, slot 9 gone
    CHECK(pm.peers[0].steamId == 11 && pm.peers[2].steamId == 55);
    { PeerMapMsg dec; auto b = Encode(pm); CHECK(Decode(b.data(), b.size(), dec) && dec.peers.size() == 3); }   // what it makes the wire accepts
    CHECK(MakePeerMap({}).peers.empty());

    // MeshPeers: a client in slot 2, the host is Steam 10.
    MeshPeers mp;
    PeerMapMsg a; a.peers = {pe(1, 11), pe(2, 22), pe(3, 33), pe(0, 10)};
    MeshPeers::Change c = mp.Apply(a, 2, 22, 10);
    CHECK(c.want.size() == 2 && c.want[0] == 11 && c.want[1] == 33 && c.drop.empty());   // not itself, not the host
    {   // the host's Steam ID under a client slot (a damaged or hostile map): never a mesh peer, the host is reached already
        MeshPeers h; PeerMapMsg hm; hm.peers = {pe(4, 10), pe(1, 11)};
        MeshPeers::Change hc = h.Apply(hm, 2, 22, 10);
        CHECK(hc.want.size() == 1 && hc.want[0] == 11 && !h.Has(10));
    }
    mp.Bind(11, 5); mp.Bind(33, 6); mp.Bind(44, 7); mp.Bind(10, 8);
    CHECK(mp.SlotOfPeer(5) == 1 && mp.SlotOfPeer(6) == 3);
    // A client finds another player's fighter by SteamID for their claws: it has no roster of its own.
    CHECK(mp.SlotOfSteam(11) == 1 && mp.SlotOfSteam(33) == 3);
    CHECK(mp.SlotOfSteam(22) == 0xFF && mp.SlotOfSteam(10) == 0xFF && mp.SlotOfSteam(99) == 0xFF);   // this machine, the host, a stranger
    CHECK(mp.SlotOfPeer(7) == 0xFF && mp.SlotOfPeer(8) == 0xFF && mp.SlotOfPeer(99) == 0xFF);   // unnamed peers, the host, strangers
    CHECK(mp.Peers().size() == 2 && mp.Named() == 2);
    PeerMapMsg b2; b2.peers = {pe(3, 33), pe(4, 44)};                  // slot 1 left, slot 4 joined
    c = mp.Apply(b2, 2, 22, 10);
    CHECK(c.want.size() == 2 && c.drop.size() == 1 && c.drop[0] == 11);
    CHECK(mp.SlotOfPeer(5) == 0xFF && mp.SlotOfPeer(6) == 3 && mp.Peers().size() == 1);   // the dropped peer streams no more
    mp.Bind(44, 9); CHECK(mp.SlotOfPeer(9) == 4 && mp.IsBound(44));
    PeerMapMsg b3; b3.peers = {pe(3, 55)};                             // slot 3's player was replaced
    c = mp.Apply(b3, 2, 22, 10);
    CHECK(c.drop.size() == 2 && c.want.size() == 1 && c.want[0] == 55);
    CHECK(mp.SlotOfPeer(6) == 0xFF);                                   // the old player's peer does not inherit slot 3
    mp.Bind(55, 12); CHECK(mp.SlotOfPeer(12) == 3);
    mp.ForgetPeer(12); mp.Bind(55, 14); CHECK(mp.SlotOfPeer(12) == 0xFF && mp.SlotOfPeer(14) == 3);   // the old peer's late Disconnected
    mp.ForgetPeer(14); mp.Bind(55, 12);
    mp.Forget(55); CHECK(mp.SlotOfPeer(12) == 0xFF && mp.Has(55) && !mp.IsBound(55));   // member left: named, to be allowed again
    mp.Bind(55, 13); CHECK(mp.SlotOfPeer(13) == 3);
    PeerMapMsg dup; dup.peers = {pe(1, 11), pe(1, 12), pe(3, 11)};     // a map naming a slot or a Steam ID twice keeps the first
    c = mp.Apply(dup, 2, 22, 10); CHECK(c.want.size() == 1 && c.want[0] == 11);
    mp.Apply(PeerMapMsg{}, 2, 22, 10); CHECK(mp.Named() == 0 && mp.Peers().empty());
    mp.Apply(a, 2, 22, 10); mp.Clear(); CHECK(mp.Named() == 0 && mp.SlotOfPeer(5) == 0xFF);

    // A direct Pose or Vitals from a mesh peer passes only for the entity that peer's slot owns.
    mp.Apply(a, 2, 22, 10); mp.Bind(11, 5);
    const uint8_t from = mp.SlotOfPeer(5);
    CHECK(MayStream(from, 2) && !MayStream(from, 3) && !MayStream(from, 1) && !MayStream(from, uint8_t(kMaxFighters + 1)));
    CHECK(!MayStream(mp.SlotOfPeer(77), 1) && !MayStream(mp.SlotOfPeer(77), 2));   // a stranger (0xFF) owns nothing

    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}
