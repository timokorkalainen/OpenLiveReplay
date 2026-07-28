#include "playback/gpu/gpusurfacelease.h"

DeadDeviceToken forbiddenTokenMint() {
    return mintDeadDeviceTokenFromFrameOp(1);
}
