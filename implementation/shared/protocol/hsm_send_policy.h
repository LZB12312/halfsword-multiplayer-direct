// Which sends flush Steam's Nagle queue for their peer. Steam holds a message up to 5 ms to batch it with later ones; a
// NoNagle message goes now, with everything queued for that peer. A blow's messages go now, as they are few
// (combat/hsm_claim.h kClaimsPerSecond) and late hits are what players feel. Other reliable traffic comes in bursts (a
// joiner's ~200 BlobChunks and its lease grants, PaintFx at up to 180/s) and keeps Steam's batching: flushing each of
// those would send a packet per message, and it still leaves within 5 ms or with the next stream message. Unreliable
// streams go now, one message per frame per fighter.
#pragma once
#include "hsm_protocol.h"
#include <cstddef>
#include <cstdint>

namespace hsm {

inline bool SendFlushes(const uint8_t* d, size_t n, bool reliable) {
    if (!reliable) return true;
    MsgType t;
    if (!PeekType(d, n, t)) return false;
    switch (t) {
        case MsgType::HitClaim: case MsgType::HitFx: case MsgType::SeverClaim: case MsgType::SeverNotice: case MsgType::StuckBlade:
            return true;
        default: return false;
    }
}

}  // namespace hsm
