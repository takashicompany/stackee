// ホストビルド用の sdkconfig.h の代役。
//
// stackee_console.c は CONFIG_MBEDTLS_HARDWARE_SHA / _MPI の有無を
// #ifdef で見るために sdkconfig.h を読む。実機のビルドでは
// build/config/sdkconfig.h が使われるが、ホストビルド (tools/test_*_host.py)
// には無いので、ここに空の代役を置く。
//
// ★ 何も define しない。ホスト側で見ているのは枠の組み立てだけで、
//   ハードウェア暗号の有無 (hw_sha / hw_mpi) は実機でしか意味が無い。
#pragma once
