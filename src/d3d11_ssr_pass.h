#pragma once

#include "d3d11_post_types.h"

namespace routeb {

void ResetPostWorldDepthSnapshot();
bool ExecutePostWorldPass(const PostWorldRequest& req, PostLogFn logger);

// Legacy shims
inline void ResetRouteCPostWorldDepthSnapshot() {
    ResetPostWorldDepthSnapshot();
}
inline bool ExecuteRouteCPostWorldPass(const PostWorldRequest& req, PostLogFn logger) {
    return ExecutePostWorldPass(req, logger);
}

}  // namespace routeb
