#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBC_PAYLOAD_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBC_PAYLOAD_H_

#include "libs/libs.h"

namespace Libs {

namespace LibcPayload {

LIB_DEFINE(InitLibcPayload_1);

[[noreturn]] KYTY_SYSV_ABI void ExitProcessNow(int code);

} // namespace LibcPayload

} // namespace Libs

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBC_PAYLOAD_H_ */
