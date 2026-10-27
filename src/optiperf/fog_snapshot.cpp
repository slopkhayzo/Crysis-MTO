// The fog volumes' math, transcribed from Cry3DEngine's machine code (see fog_snapshot.h). Every expression keeps the
// code's operations in its order (x64 MSVC /fp:precise keeps them), and every comparison the code's behaviour for
// unordered operands: where a decompiler shows `a <= b`, the code branches on whether b > a, which differs for NaN.
// The decompiler also reassociates some products (the box's step along x), so the order here is the machine code's.

#include "fog_snapshot.h"

#include <cmath>
#include <cstring>

#include "scan.h"

namespace optiperf::fog {
namespace {

// --- the code this follows (x64 Cry3DEngine.dll, build 6156) --------------------------------------------------------
// the ellipsoid's contribution (Cry3DEngine +0x9DD80, called by the loop); 1595 bytes
constexpr const char kEllipsoidCode[] =
    "48 8B C4 48 89 58 08 57 48 81 EC D0 00 00 00 F3 0F 10 1D ?? ?? ?? ?? F3 0F 10 25 ?? ?? ?? ?? F3 "
    "0F 10 2D ?? ?? ?? ?? 44 0F 29 58 98 44 0F 29 60 88 44 0F 29 7C 24 30 48 8B DA 48 8B F9 F3 45 0F "
    "10 38 F3 45 0F 10 58 04 F3 45 0F 10 60 08 F3 44 0F 5C FB F3 44 0F 5C DC F3 44 0F 5C E5 41 0F 28 "
    "C7 41 0F 28 D3 41 0F 28 CC F3 41 0F 59 D3 F3 41 0F 59 C7 F3 41 0F 59 CC F3 0F 58 D0 F3 0F 58 D1 "
    "0F 2F 15 ?? ?? ?? ?? 0F 86 8E 05 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 48 B8 44 "
    "0F 29 50 A8 0F 28 C3 F3 0F 59 81 98 00 00 00 0F 28 CD 44 0F 28 D4 44 0F 28 CC F3 0F 59 89 A0 00 "
    "00 00 F3 44 0F 59 91 9C 00 00 00 F3 44 0F 59 89 AC 00 00 00 44 0F 28 C4 41 0F 28 FB 44 0F 29 6C "
    "24 50 F3 44 0F 10 2D ?? ?? ?? ?? F3 44 0F 58 D0 F3 44 0F 58 D1 0F 28 C3 44 0F 29 74 24 40 0F 28 "
    "CD F3 44 0F 58 91 A4 00 00 00 F3 0F 59 81 A8 00 00 00 F3 0F 59 89 B0 00 00 00 F3 44 0F 59 81 BC "
    "00 00 00 45 0F 28 F3 41 0F 28 F3 F3 44 0F 58 C8 0F 28 C3 F3 44 0F 58 C9 0F 28 CD F3 44 0F 58 89 "
    "B4 00 00 00 F3 0F 59 81 B8 00 00 00 F3 0F 59 89 C0 00 00 00 F3 0F 59 B9 9C 00 00 00 F3 44 0F 58 "
    "C0 41 0F 28 C7 F3 44 0F 58 C1 41 0F 28 CC F3 44 0F 58 81 C4 00 00 00 F3 0F 59 81 98 00 00 00 F3 "
    "0F 59 89 A0 00 00 00 F3 44 0F 59 B1 AC 00 00 00 F3 0F 58 F8 41 0F 28 C7 F3 0F 58 F9 41 0F 28 CC "
    "F3 0F 59 81 A8 00 00 00 F3 0F 59 89 B0 00 00 00 F3 0F 59 B1 BC 00 00 00 F3 44 0F 58 F0 41 0F 28 "
    "C7 F3 44 0F 58 F1 41 0F 28 CC F3 0F 59 81 B8 00 00 00 F3 0F 59 89 C0 00 00 00 F3 45 0F 59 D5 F3 "
    "0F 58 F0 41 0F 28 C6 F3 0F 58 F1 0F 28 CF F3 41 0F 59 C6 F3 0F 59 CF F3 45 0F 59 CD 0F 28 D6 F3 "
    "0F 58 C1 F3 0F 59 D6 F3 45 0F 59 C5 F3 0F 58 C2 E8 ?? ?? ?? ?? F3 0F 11 84 24 F0 00 00 00 0F 28 "
    "C8 F3 0F 10 05 ?? ?? ?? ?? F3 0F 5E C1 F3 0F 59 F8 F3 44 0F 59 F0 F3 0F 59 F0 F3 0F 11 44 24 20 "
    "F3 45 0F 59 F1 F3 41 0F 59 FA F3 45 0F 59 C9 F3 44 0F 58 F7 F3 41 0F 59 F0 F3 44 0F 58 F6 F3 41 "
    "0F 10 F6 F3 45 0F 59 D2 F3 45 0F 59 C0 F3 45 0F 58 CA F3 45 0F 58 C8 F3 44 0F 59 F6 F3 44 0F 5C "
    "0D ?? ?? ?? ?? F3 45 0F 5C F1 45 0F 57 C9 45 0F 2F F1 0F 82 63 03 00 00 41 0F 28 C6 E8 ?? ?? ?? "
    "?? F3 44 0F 10 15 ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? F3 44 0F 11 8C 24 F8 00 00 00 0F 28 E8 F3 "
    "0F 10 44 24 20 F3 44 0F 59 F8 F3 44 0F 59 D8 F3 44 0F 59 E0 41 0F 28 C2 F3 0F 5C C6 F3 0F 5C C5 "
    "F3 0F 59 C2 44 0F 2F C8 F3 0F 11 44 24 20 76 0A 48 8D 84 24 F8 00 00 00 EB 05 48 8D 44 24 20 F3 "
    "0F 10 08 F3 0F 5C EE F3 44 0F 11 8C 24 F8 00 00 00 F3 0F 59 EA 44 0F 2F CD F3 0F 11 4C 24 24 F3 "
    "0F 11 6C 24 20 76 0A 48 8D 84 24 F8 00 00 00 EB 05 48 8D 44 24 20 F3 0F 10 00 F3 0F 10 94 24 F0 "
    "00 00 00 0F 2F D0 F3 0F 11 84 24 F8 00 00 00 76 0A 48 8D 84 24 F8 00 00 00 EB 08 48 8D 84 24 F0 "
    "00 00 00 F3 0F 10 00 0F 2F C1 77 05 48 8D 44 24 24 F3 0F 10 00 45 0F 28 C3 41 0F 28 F4 F3 0F 5C "
    "C1 41 0F 28 FF F3 44 0F 59 C1 F3 0F 59 F1 F3 44 0F 58 05 ?? ?? ?? ?? F3 0F 58 35 ?? ?? ?? ?? 0F "
    "28 D0 F3 44 0F 59 E0 F3 41 0F 59 D7 F3 44 0F 59 D8 41 0F 28 C3 F3 0F 11 94 24 F0 00 00 00 F3 0F "
    "59 F9 F3 41 0F 59 C3 0F 28 CA F3 0F 58 3D ?? ?? ?? ?? F3 0F 59 CA 41 0F 28 D4 F3 41 0F 59 D4 F3 "
    "0F 58 C1 F3 0F 58 C2 E8 ?? ?? ?? ?? F3 44 0F 5C 87 50 01 00 00 F3 0F 5C BF 4C 01 00 00 44 0F 28 "
    "F8 41 0F 28 C2 F3 44 0F 59 87 38 01 00 00 F3 0F 59 BF 34 01 00 00 F3 0F 5C B7 54 01 00 00 F3 44 "
    "0F 58 C7 F3 0F 10 3D ?? ?? ?? ?? F3 0F 59 B7 3C 01 00 00 F3 44 0F 58 C6 F3 0F 10 35 ?? ?? ?? ?? "
    "F3 41 0F 5C C0 0F 2F F0 76 05 0F 28 C6 EB 08 0F 2F F8 77 03 0F 28 C7 E8 ?? ?? ?? ?? F3 44 0F 59 "
    "9F 38 01 00 00 F3 44 0F 59 A7 3C 01 00 00 44 0F 28 C0 F3 0F 10 84 24 F0 00 00 00 F3 0F 59 87 34 "
    "01 00 00 F3 45 0F 59 C7 F3 44 0F 58 D8 F3 45 0F 58 DC F3 41 0F 5A C3 66 0F 54 05 ?? ?? 12 00 F2 "
    "0F 5A C0 0F 2F 05 ?? ?? ?? ?? 76 3D 41 0F 28 C2 F3 41 0F 5C C3 0F 2F F0 76 05 0F 28 C6 EB 08 0F "
    "2F F8 77 03 0F 28 C7 E8 ?? ?? ?? ?? F3 44 0F 10 25 ?? ?? ?? ?? 41 0F 28 CC F3 0F 5C C8 F3 41 0F "
    "5E CB F3 44 0F 59 C1 EB 09 F3 44 0F 10 25 ?? ?? ?? ?? F3 44 0F 59 B7 B8 01 00 00 F3 44 0F 58 B7 "
    "BC 01 00 00 45 0F 2F CE 77 10 45 0F 2F E6 76 06 45 0F 28 CE EB 04 45 0F 28 CC F3 45 0F 5C E9 F3 "
    "45 0F 59 E9 F3 45 0F 59 E8 F3 44 0F 59 AF 08 01 00 00 F3 45 0F 5C D5 41 0F 2F F2 77 0F 41 0F 2F "
    "FA 76 06 41 0F 28 F2 EB 03 0F 28 F7 0F 28 C6 E8 ?? ?? ?? ?? F3 44 0F 11 A4 24 F0 00 00 00 41 0F "
    "2F C4 F3 0F 11 84 24 F8 00 00 00 76 0A 48 8D 8C 24 F0 00 00 00 EB 08 48 8D 8C 24 F8 00 00 00 8B "
    "87 C0 01 00 00 89 03 8B 87 C4 01 00 00 89 43 04 8B 87 C8 01 00 00 89 43 08 8B 01 89 43 0C 48 8B "
    "C3 44 0F 28 6C 24 50 44 0F 28 94 24 80 00 00 00 44 0F 28 8C 24 90 00 00 00 44 0F 28 84 24 A0 00 "
    "00 00 0F 28 BC 24 B0 00 00 00 0F 28 B4 24 C0 00 00 00 44 0F 28 74 24 40 48 8B 9C 24 E0 00 00 00 "
    "44 0F 28 5C 24 70 44 0F 28 64 24 60 44 0F 28 7C 24 30 48 81 C4 D0 00 00 00 5F C3 C7 03 00 00 80 "
    "3F C7 43 04 00 00 80 3F C7 43 08 00 00 80 3F C7 43 0C 00 00 80 3F 48 8B C3 EB 86 C7 02 00 00 80 "
    "3F C7 42 04 00 00 80 3F C7 42 08 00 00 80 3F C7 42 0C 00 00 80 3F 48 8B C2 EB 9D ";

// the box's contribution (+0x9E3C0); 1874 bytes
constexpr const char kBoxCode[] =
    "48 8B C4 48 89 58 08 57 48 81 EC E0 00 00 00 F3 0F 10 1D ?? ?? ?? ?? F3 0F 10 25 ?? ?? ?? ?? F3 "
    "0F 10 2D ?? ?? ?? ?? 44 0F 29 48 B8 44 0F 29 50 A8 44 0F 29 58 98 48 8B DA 48 8B F9 F3 45 0F 10 "
    "18 F3 45 0F 10 48 04 F3 45 0F 10 50 08 F3 44 0F 5C DB F3 44 0F 5C CC F3 44 0F 5C D5 41 0F 28 C3 "
    "41 0F 28 D1 41 0F 28 CA F3 41 0F 59 D1 F3 41 0F 59 C3 F3 41 0F 59 CA F3 0F 58 D0 F3 0F 58 D1 0F "
    "2F 15 ?? ?? ?? ?? 0F 86 A3 06 00 00 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 44 0F 29 60 88 44 0F "
    "29 6C 24 60 44 0F 29 74 24 50 44 0F 29 7C 24 40 0F 28 C3 0F 28 CD F3 0F 59 81 98 00 00 00 F3 0F "
    "59 89 A0 00 00 00 0F 28 D4 44 0F 28 F4 44 0F 28 FC F3 0F 59 91 BC 00 00 00 F3 44 0F 59 B1 9C 00 "
    "00 00 F3 44 0F 59 B9 AC 00 00 00 45 0F 28 C1 41 0F 28 F9 45 0F 28 E9 F3 44 0F 58 F0 0F 28 C3 F3 "
    "44 0F 58 F1 0F 28 CD F3 44 0F 58 B1 A4 00 00 00 F3 0F 59 81 A8 00 00 00 F3 0F 59 89 B0 00 00 00 "
    "F3 44 0F 59 81 9C 00 00 00 F3 44 0F 58 F8 0F 28 C3 F3 44 0F 58 F9 0F 28 CD F3 44 0F 58 B9 B4 00 "
    "00 00 F3 0F 59 81 B8 00 00 00 F3 0F 59 89 C0 00 00 00 F3 0F 59 B9 AC 00 00 00 F3 0F 58 D0 41 0F "
    "28 C3 F3 0F 58 D1 41 0F 28 CA F3 0F 58 91 C4 00 00 00 F3 0F 59 81 98 00 00 00 F3 0F 59 89 A0 00 "
    "00 00 F3 44 0F 59 A9 BC 00 00 00 F3 0F 11 54 24 30 F3 44 0F 58 C0 41 0F 28 C3 F3 44 0F 58 C1 41 "
    "0F 28 CA F3 0F 59 81 A8 00 00 00 F3 0F 59 89 B0 00 00 00 F3 0F 58 F8 41 0F 28 C3 F3 0F 58 F9 F3 "
    "0F 59 81 B8 00 00 00 41 0F 28 CA F3 0F 59 89 C0 00 00 00 F3 44 0F 58 E8 0F 28 C7 F3 0F 59 C7 F3 "
    "44 0F 58 E9 41 0F 28 C8 F3 41 0F 59 C8 41 0F 28 D5 F3 41 0F 59 D5 F3 0F 58 C1 F3 0F 58 C2 E8 ?? "
    "?? ?? ?? 0F 57 DB F3 44 0F 10 25 ?? ?? ?? ?? F3 0F 10 25 ?? ?? ?? ?? 41 0F 28 EC F3 0F 11 9C 24 "
    "08 01 00 00 0F 28 CB F3 0F 5E E8 F3 0F 11 44 24 28 F3 0F 11 A4 24 00 01 00 00 F3 44 0F 59 C5 F3 "
    "0F 59 FD F3 44 0F 59 ED F3 0F 11 6C 24 2C F2 0F 10 15 ?? ?? ?? ?? F3 0F 10 35 ?? ?? ?? ?? F3 41 "
    "0F 5A C0 66 0F 54 C2 F2 0F 5A C0 0F 2F C3 0F 86 9D 00 00 00 44 0F 2F C3 41 0F 28 C4 41 0F 28 D4 "
    "F3 41 0F 5C D6 0F 28 CE F3 41 0F 5E C0 F3 41 0F 5C CE F3 0F 59 D0 F3 0F 59 C8 76 0B F3 0F 11 4C "
    "24 20 0F 28 C1 EB 09 F3 0F 11 54 24 20 0F 28 C2 44 0F 2F C3 76 08 F3 0F 11 54 24 24 EB 09 F3 0F "
    "11 4C 24 24 0F 28 D1 0F 2F C3 76 07 48 8D 44 24 20 EB 08 48 8D 84 24 08 01 00 00 0F 2F E2 F3 0F "
    "10 08 F3 0F 11 8C 24 08 01 00 00 76 07 48 8D 44 24 24 EB 08 48 8D 84 24 00 01 00 00 F3 0F 10 20 "
    "F2 0F 10 15 ?? ?? ?? ?? F3 0F 11 A4 24 00 01 00 00 F3 0F 5A C7 66 0F 54 C2 F2 0F 5A C0 0F 2F C3 "
    "0F 86 A0 00 00 00 0F 2F FB 41 0F 28 C4 41 0F 28 D4 F3 41 0F 5C D7 0F 28 EE F3 0F 5E C7 F3 41 0F "
    "5C EF F3 0F 59 D0 F3 0F 59 E8 76 0B F3 0F 11 6C 24 24 0F 28 C5 EB 09 F3 0F 11 54 24 24 0F 28 C2 "
    "0F 2F FB 76 08 F3 0F 11 54 24 20 EB 09 F3 0F 11 6C 24 20 0F 28 D5 0F 2F C1 76 07 48 8D 44 24 24 "
    "EB 08 48 8D 84 24 08 01 00 00 0F 2F E2 F3 0F 10 08 F3 0F 11 8C 24 08 01 00 00 76 07 48 8D 44 24 "
    "20 EB 08 48 8D 84 24 00 01 00 00 F3 0F 10 20 F3 0F 10 6C 24 2C F2 0F 10 15 ?? ?? ?? ?? F3 0F 11 "
    "A4 24 00 01 00 00 44 0F 28 7C 24 40 F3 41 0F 5A C5 66 0F 54 C2 F2 0F 5A C0 0F 2F C3 0F 86 96 00 "
    "00 00 44 0F 2F EB F3 0F 10 7C 24 30 41 0F 28 C4 41 0F 28 D4 F3 41 0F 5E C5 F3 0F 5C D7 F3 0F 5C "
    "F7 F3 0F 59 D0 F3 0F 59 F0 76 0B F3 0F 11 74 24 24 0F 28 C6 EB 09 F3 0F 11 54 24 24 0F 28 C2 44 "
    "0F 2F EB 76 08 F3 0F 11 54 24 20 EB 09 F3 0F 11 74 24 20 0F 28 D6 0F 2F C1 76 07 48 8D 44 24 24 "
    "EB 08 48 8D 84 24 08 01 00 00 0F 2F E2 F3 0F 10 08 F3 0F 11 8C 24 08 01 00 00 76 07 48 8D 44 24 "
    "20 EB 08 48 8D 84 24 00 01 00 00 F3 0F 10 20 F3 0F 11 A4 24 00 01 00 00 0F 2F DC F3 0F 11 5C 24 "
    "30 76 07 48 8D 44 24 30 EB 08 48 8D 84 24 00 01 00 00 F3 0F 10 00 0F 2F C8 F3 0F 11 84 24 00 01 "
    "00 00 76 69 C7 03 00 00 80 3F C7 43 04 00 00 80 3F C7 43 08 00 00 80 3F C7 43 0C 00 00 80 3F 48 "
    "8B C3 44 0F 28 6C 24 60 44 0F 28 64 24 70 44 0F 28 84 24 B0 00 00 00 0F 28 BC 24 C0 00 00 00 0F "
    "28 B4 24 D0 00 00 00 44 0F 28 74 24 50 4C 8D 9C 24 E0 00 00 00 49 8B 5B 10 45 0F 28 4B C0 45 0F "
    "28 53 B0 45 0F 28 5B A0 49 8B E3 5F C3 F3 0F 10 54 24 28 F3 44 0F 59 DD F3 44 0F 59 CD 0F 2F D0 "
    "F3 44 0F 59 D5 76 0A 48 8D 84 24 00 01 00 00 EB 05 48 8D 44 24 28 F3 0F 10 00 0F 2F C1 77 08 48 "
    "8D 84 24 08 01 00 00 F3 0F 10 00 45 0F 28 C1 41 0F 28 F2 F3 0F 5C C1 41 0F 28 FB F3 44 0F 59 C1 "
    "F3 0F 59 F1 F3 44 0F 58 05 ?? ?? ?? ?? F3 0F 58 35 ?? ?? ?? ?? 44 0F 28 E8 F3 44 0F 59 D0 F3 44 "
    "0F 59 C8 F3 0F 59 F9 41 0F 28 C1 41 0F 28 D2 F3 0F 58 3D ?? ?? ?? ?? F3 41 0F 59 C1 F3 45 0F 59 "
    "EB F3 41 0F 59 D2 41 0F 28 CD F3 41 0F 59 CD F3 0F 58 C1 F3 0F 58 C2 E8 ?? ?? ?? ?? F3 44 0F 5C "
    "87 50 01 00 00 F3 0F 5C B7 54 01 00 00 44 0F 28 F0 F3 44 0F 59 87 38 01 00 00 F3 0F 59 B7 3C 01 "
    "00 00 F3 0F 5C BF 4C 01 00 00 F3 44 0F 10 1D ?? ?? ?? ?? F3 0F 59 BF 34 01 00 00 41 0F 28 C3 F3 "
    "44 0F 58 C7 F3 44 0F 58 C6 F3 0F 10 35 ?? ?? ?? ?? F3 41 0F 5C C0 F3 44 0F 10 05 ?? ?? ?? ?? 0F "
    "2F F0 76 05 0F 28 C6 EB 0A 44 0F 2F C0 77 04 41 0F 28 C0 E8 ?? ?? ?? ?? F3 44 0F 59 8F 38 01 00 "
    "00 F3 44 0F 59 AF 34 01 00 00 0F 28 F8 F3 45 0F 58 CD F3 44 0F 59 97 3C 01 00 00 F3 41 0F 59 FE "
    "F3 45 0F 58 CA F3 41 0F 5A C1 66 0F 54 05 ?? ?? 12 00 F2 0F 5A C0 0F 2F 05 ?? ?? ?? ?? 76 33 41 "
    "0F 28 C3 F3 41 0F 5C C1 0F 2F F0 76 05 0F 28 C6 EB 0A 44 0F 2F C0 77 04 41 0F 28 C0 E8 ?? ?? ?? "
    "?? 41 0F 28 CC F3 0F 5C C8 F3 41 0F 5E C9 F3 0F 59 F9 F3 0F 59 BF 08 01 00 00 F3 44 0F 5C DF 41 "
    "0F 2F F3 77 10 45 0F 2F C3 76 06 41 0F 28 F3 EB 04 41 0F 28 F0 0F 28 C6 E8 ?? ?? ?? ?? F3 44 0F "
    "11 A4 24 00 01 00 00 41 0F 2F C4 F3 0F 11 84 24 08 01 00 00 76 0A 48 8D 8C 24 00 01 00 00 EB 08 "
    "48 8D 8C 24 08 01 00 00 8B 87 C0 01 00 00 89 03 8B 87 C4 01 00 00 89 43 04 8B 87 C8 01 00 00 89 "
    "43 08 8B 01 89 43 0C 48 8B C3 E9 93 FD FF FF C7 02 00 00 80 3F C7 42 04 00 00 80 3F C7 42 08 00 "
    "00 80 3F C7 42 0C 00 00 80 3F 48 8B C2 E9 9B FD FF FF ";

// the ray-box test (+0x7DA0): reads only its arguments, writes only the hit point; 651 bytes
constexpr const char kRayBoxCode[] =
    "48 89 5C 24 20 57 48 83 EC 10 F3 0F 10 09 49 8B F8 48 8B D9 0F 2F 0A 72 04 B0 01 EB 02 32 C0 F3 "
    "0F 10 42 0C 45 33 C9 0F 2F C1 45 8D 51 01 72 05 41 8B CA EB 03 41 8B C9 F3 0F 10 4B 04 02 C9 0A "
    "C1 0F 2F 4A 04 72 05 41 8B CA EB 03 41 8B C9 F3 0F 10 42 10 C0 E1 02 0A C1 0F 2F C1 72 05 41 8B "
    "CA EB 03 41 8B C9 F3 0F 10 4B 08 C0 E1 03 0A C1 0F 2F 4A 08 72 05 41 8B CA EB 03 41 8B C9 F3 0F "
    "10 42 14 C0 E1 04 0A C1 0F 2F C1 72 05 41 8B CA EB 03 41 8B C9 C0 E1 05 0A C8 80 F9 3F 75 20 8B "
    "03 41 89 00 8B 43 04 41 89 40 04 8B 43 08 41 89 40 08 B0 02 48 8B 5C 24 38 48 83 C4 10 5F C3 0F "
    "57 E4 48 89 6C 24 20 4C 89 64 24 30 4C 8D 24 24 48 8B EA 4C 8B DA 48 2B EB 4C 8D 43 0C 48 89 74 "
    "24 28 4C 2B E2 66 66 66 90 66 66 90 66 66 66 90 F3 41 0F 10 08 0F 2F CC 0F 86 99 00 00 00 F3 41 "
    "0F 10 58 F4 F3 41 0F 10 03 0F 2F C3 0F 86 85 00 00 00 F3 43 0F 11 04 1C 0F 28 D0 41 8D 42 FD F3 "
    "0F 5C D3 48 63 F0 49 63 C2 48 C1 FE 1F 48 23 F0 41 8D 42 FE 48 63 C8 F3 0F 5E D1 48 C1 F9 1F 83 "
    "E1 03 4A 8D 04 09 0F 28 C2 F3 0F 59 44 B3 0C F3 0F 59 54 83 08 F3 0F 58 04 B3 F3 0F 58 54 83 FC "
    "F3 0F 11 04 B4 F3 0F 11 54 84 FC F3 0F 10 1C B4 0F 2F 1C B2 76 21 F3 0F 10 44 B2 0C 0F 2F C3 76 "
    "16 0F 2F 54 82 FC 76 0F F3 0F 10 44 82 08 0F 2F C2 0F 87 DC 00 00 00 0F 2F E1 0F 86 9D 00 00 00 "
    "F3 41 0F 10 50 F4 41 0F 2F 14 28 0F 86 8C 00 00 00 F3 41 0F 10 04 28 41 8D 42 FD F3 0F 5C D0 F3 "
    "43 0F 11 04 1C 48 63 F0 49 63 C2 48 C1 FE 1F 48 23 F0 41 8D 42 FE 48 63 C8 F3 0F 5E D1 48 C1 F9 "
    "1F 83 E1 03 F3 0F 10 04 B3 4A 8D 04 09 0F 28 CA F3 0F 59 54 83 08 F3 0F 59 4C B3 0C F3 0F 5C C1 "
    "F3 0F 10 4C 83 FC F3 0F 5C CA F3 0F 11 04 B4 F3 0F 11 4C 84 FC F3 0F 10 14 B4 0F 2F 14 B2 76 1D "
    "F3 0F 10 44 B2 0C 0F 2F C2 76 12 0F 2F 4C 82 FC 76 0B F3 0F 10 44 82 08 0F 2F C1 77 36 49 83 C1 "
    "01 41 83 C2 01 49 83 C3 04 49 83 C0 04 49 83 F9 03 0F 8C 99 FE FF FF 32 C0 48 8B 74 24 28 48 8B "
    "6C 24 20 4C 8B 64 24 30 48 8B 5C 24 38 48 83 C4 10 5F C3 48 8D 0C 24 8B 01 89 07 8B 41 04 89 47 "
    "04 8B 41 08 89 47 08 B0 01 EB CE ";

// TraceFogVolumes' loop (+0x9F7AF, 0x17F into the function): the cvars, the camera box, the list, the calls; 611 bytes
constexpr const char kLoopCode[] =
    "48 8B 05 ?? ?? ?? ?? 44 39 70 30 0F 84 4D 02 00 00 44 39 70 34 0F 84 43 02 00 00 F3 0F 10 05 ?? "
    "?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? 48 8D 4C 24 20 48 8D 54 24 60 48 89 B4 "
    "24 E0 00 00 00 48 8B 35 ?? ?? ?? ?? 4C 89 A4 24 C0 00 00 00 4C 8B 25 ?? ?? ?? ?? F3 0F 58 0D ?? "
    "?? ?? ?? F3 0F 10 1D ?? ?? ?? ?? F3 0F 58 15 ?? ?? ?? ?? 49 3B F4 F3 0F 59 C8 F3 0F 59 D0 F3 0F "
    "58 1D ?? ?? ?? ?? F3 0F 11 4C 24 20 F3 0F 11 54 24 24 F3 0F 59 D8 F3 0F 10 45 00 F3 0F 11 5C 24 "
    "28 48 8B 01 48 89 02 8B 41 08 48 8D 4C 24 30 F3 0F 5C C1 F3 0F 10 4D 04 89 42 08 48 8D 54 24 6C "
    "F3 0F 11 44 24 30 F3 0F 10 45 08 F3 0F 5C CA F3 0F 11 4C 24 34 F3 0F 5C C3 F3 0F 11 44 24 38 8B "
    "01 89 02 8B 41 04 89 42 04 8B 41 08 89 42 08 0F 84 16 01 00 00 66 66 66 90 66 66 66 90 66 66 66 "
    "90 48 8B 3E 0F BA 67 0C 08 0F 82 EF 00 00 00 48 8D 97 A0 01 00 00 4C 8D 44 24 30 48 8D 4C 24 60 "
    "E8 ?? ?? ?? ?? 84 C0 0F 84 D1 00 00 00 44 39 B7 C8 00 00 00 75 2D 48 8D 54 24 78 4C 8B C5 48 8B "
    "CF E8 ?? ?? ?? ?? 48 8D 54 24 20 8B 08 89 0A 8B 48 04 89 4A 04 8B 48 08 8B 40 0C 89 4A 08 89 42 "
    "0C EB 2E 48 8D 94 24 88 00 00 00 4C 8B C5 48 8B CF E8 ?? ?? ?? ?? 48 8D 54 24 20 8B 08 89 0A 8B "
    "48 04 89 4A 04 8B 48 08 8B 40 0C 89 4A 08 89 42 0C F3 0F 10 4C 24 20 0F 28 D6 F3 0F 5C 0B F3 0F "
    "5C 54 24 2C F3 0F 59 CA F3 0F 58 0B F3 0F 11 54 24 2C F3 0F 11 0B F3 0F 10 4C 24 24 F3 0F 5C 4B "
    "04 F3 0F 59 CA F3 0F 58 4B 04 F3 0F 11 4B 04 F3 0F 10 4C 24 28 F3 0F 5C 4B 08 F3 0F 59 CA F3 0F "
    "58 4B 08 F3 0F 11 4B 08 0F 28 CE F3 0F 5C 4B 0C F3 0F 59 CA F3 0F 58 4B 0C F3 0F 11 4B 0C 48 83 "
    "C6 10 49 3B F4 0F 85 F6 FE FF FF F3 0F 10 53 0C 4C 8B A4 24 C0 00 00 00 48 8B B4 24 E0 00 00 00 "
    "0F 2F 15 ?? ?? ?? ?? 76 2A F3 0F 10 03 F3 0F 10 4B 04 F3 0F 5E C2 F3 0F 5E CA F3 0F 11 03 F3 0F "
    "10 43 08 F3 0F 11 4B 04 F3 0F 5E C2 F3 0F 11 43 08 EB 0B 44 89 33 44 89 73 04 44 89 73 08 F3 0F "
    "5C 73 0C ";

// TraceFogVolumes' loop (kLoopCode): the fields it reads (rip-relative displacements) and the functions it calls
// (rel32 fields)
constexpr int kLoopCvars = 0x3, kLoopMaxX = 0x27, kLoopMaxY = 0x2F, kLoopBegin = 0x48, kLoopEnd = 0x57;
constexpr int kLoopMinX = 0x5F, kLoopMaxZ = 0x67, kLoopMinY = 0x6F, kLoopMinZ = 0x82;
constexpr int kLoopRayBox = 0x121, kLoopEllipsoid = 0x142, kLoopBox = 0x172;
constexpr size_t kLoopSearch = 0x300;  // how far into TraceFogVolumes the loop is looked for
// in both contributions: the camera position's three floats; their calls (rel32 fields), all of them to sqrtf or
// expf (the ellipsoid: the line's length, the discriminant's root, the segment's length, then the three expf; the box
// the same without the discriminant)
constexpr int kCams[3] = {0x13, 0x1B, 0x23};
constexpr int kEllipsoidSqrt[] = {0x211, 0x29D, 0x3E8}, kEllipsoidExp[] = {0x458, 0x4C8, 0x550};
constexpr int kBoxSqrt[] = {0x1FF, 0x5B8}, kBoxExp[] = {0x634, 0x69D, 0x6D9};
constexpr const char kThunk[] = "FF 25 ?? ?? ?? ??";  // jmp qword ptr [rip + disp32]: an import

// CFogVolumeRenderNode
constexpr int kFlags = 0xC, kType = 0xC8, kBounds = 0x1A0;
constexpr uint32_t kHidden = 0x100;  // a flag the loop skips the node for

inline float F(const uint8_t* n, int off) {
    float f;
    std::memcpy(&f, n + off, sizeof f);
    return f;
}
inline uint32_t U(const uint8_t* n, int off) {
    uint32_t v;
    std::memcpy(&v, n + off, sizeof v);
    return v;
}
inline void Ones(float* out) { out[0] = out[1] = out[2] = out[3] = 1.0f; }
// the clamp before each expf: [-80, 80], NaN giving 80, as the code's branches do
inline float Clamp80(float x) { return (-80.0f > x) ? -80.0f : ((80.0f > x) ? x : 80.0f); }
// the result: the node's colour (+0x1C0) and the transmittance, at most 1
inline void Colour(const uint8_t* n, float e, float* out) {
    std::memcpy(out, n + 0x1C0, 3 * sizeof(float));
    out[3] = (e > 1.0f) ? 1.0f : e;
}

// what a rip-relative or rel32 field points at
char* Target4(char* field) { return field + 4 + int32_t(ReadU32(field)); }

bool Code(char* base, char* at, const char* text) {
    const Pattern p = ParsePattern(text);
    return at && IsInCode(base, at, p.size()) && MatchesAt(at, p);
}

}  // namespace

// Cry3DEngine +0x9DD80: the line from the camera to pos through an ellipsoid, the unit sphere in the node's space
// (+0x98: world to local, 3x4)
void Ellipsoid(const uint8_t* n, const float* cam, const float* pos, float* out, const Math& m) {
    const float cx = cam[0], cy = cam[1], cz = cam[2];
    const float dx = pos[0] - cx, dy = pos[1] - cy, dz = pos[2] - cz;
    if (!(dy * dy + dx * dx + dz * dz > 0.0001f)) return Ones(out);
    const float m98 = F(n, 0x98), m9C = F(n, 0x9C), mA0 = F(n, 0xA0), mA4 = F(n, 0xA4);
    const float mA8 = F(n, 0xA8), mAC = F(n, 0xAC), mB0 = F(n, 0xB0), mB4 = F(n, 0xB4);
    const float mB8 = F(n, 0xB8), mBC = F(n, 0xBC), mC0 = F(n, 0xC0), mC4 = F(n, 0xC4);
    // the camera in the node's space, doubled, and the line's direction there
    const float c0 = (cy * m9C + cx * m98 + cz * mA0 + mA4) * 2.0f;
    const float c1 = (cy * mAC + cx * mA8 + cz * mB0 + mB4) * 2.0f;
    const float c2 = (cy * mBC + cx * mB8 + cz * mC0 + mC4) * 2.0f;
    const float v0 = dy * m9C + dx * m98 + dz * mA0;
    const float v1 = dy * mAC + dx * mA8 + dz * mB0;
    const float v2 = dy * mBC + dx * mB8 + dz * mC0;
    const float len = m.sqrt(v1 * v1 + v0 * v0 + v2 * v2);
    const float inv = 1.0f / len;
    const float b = v1 * inv * c1 + v0 * inv * c0 + v2 * inv * c2;
    const float disc = b * b - (c1 * c1 + c0 * c0 + c2 * c2 - 4.0f);
    if (!(disc >= 0.0f)) return Ones(out);  // the line misses it
    const float sq = m.sqrt(disc);
    const float ndx = dx * inv, ndy = dy * inv, ndz = dz * inv;
    // where the line enters and leaves it, within [0, len]
    const float t0 = (-b - sq) * 0.5f;
    const float enter = (0.0f > t0) ? 0.0f : t0;
    const float t1 = (sq - b) * 0.5f;
    const float leave0 = (0.0f > t1) ? 0.0f : t1;
    const float leave1 = (len > leave0) ? leave0 : len;
    const float leave = (leave1 > enter) ? leave1 : enter;
    const float seg = leave - enter;
    const float py = ndy * enter + cy, pz = ndz * enter + cz;
    const float sz = ndz * seg, sx = seg * ndx, sy = ndy * seg;
    const float px = ndx * enter + cx;
    const float slen = m.sqrt(sy * sy + sx * sx + sz * sz);
    // the height falloff (+0x134: its direction, +0x14C: its base) at the entry, integrated along the segment
    const float h =
        (py - F(n, 0x150)) * F(n, 0x138) + (px - F(n, 0x14C)) * F(n, 0x134) + (pz - F(n, 0x154)) * F(n, 0x13C);
    float f = m.exp(Clamp80(-h)) * slen;
    const float g = sy * F(n, 0x138) + sx * F(n, 0x134) + sz * F(n, 0x13C);
    if (std::fabs(g) > 0.001f) f = f * ((1.0f - m.exp(Clamp80(-g))) / g);
    // the soft edge (+0x1B8, +0x1BC) and the density (+0x108)
    const float k = disc * F(n, 0x1B8) + F(n, 0x1BC);
    const float edge = (0.0f > k) ? 0.0f : ((1.0f > k) ? k : 1.0f);
    Colour(n, m.exp(Clamp80(-((2.0f - edge) * edge * f * F(n, 0x108)))), out);
}

// Cry3DEngine +0x9E3C0: the line from the camera to pos through a box, the cube [-1, 1]^3 in the node's space
void Box(const uint8_t* n, const float* cam, const float* pos, float* out, const Math& m) {
    const float cx = cam[0], cy = cam[1], cz = cam[2];
    const float dx = pos[0] - cx, dy = pos[1] - cy, dz = pos[2] - cz;
    if (!(dy * dy + dx * dx + dz * dz > 0.0001f)) return Ones(out);
    const float m98 = F(n, 0x98), m9C = F(n, 0x9C), mA0 = F(n, 0xA0), mA4 = F(n, 0xA4);
    const float mA8 = F(n, 0xA8), mAC = F(n, 0xAC), mB0 = F(n, 0xB0), mB4 = F(n, 0xB4);
    const float mB8 = F(n, 0xB8), mBC = F(n, 0xBC), mC0 = F(n, 0xC0), mC4 = F(n, 0xC4);
    // the camera in the node's space, and the line's direction there
    const float o[3] = {cy * m9C + cx * m98 + cz * mA0 + mA4, cy * mAC + cx * mA8 + cz * mB0 + mB4,
                        cy * mBC + cx * mB8 + cz * mC0 + mC4};
    const float v0 = dy * m9C + dx * m98 + dz * mA0;
    const float v1 = dy * mAC + dx * mA8 + dz * mB0;
    const float v2 = dy * mBC + dx * mB8 + dz * mC0;
    const float len = m.sqrt(v1 * v1 + v0 * v0 + v2 * v2);
    const float inv = 1.0f / len;
    const float d[3] = {v0 * inv, v1 * inv, v2 * inv};
    // each pair of faces: where the line crosses them; the latest entry and the earliest exit
    float enter = 0.0f, leave = 3.4028235e+38f;
    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(d[i]) > 0.0f)) continue;
        const float r = 1.0f / d[i];
        const float a = (1.0f - o[i]) * r, c = (-1.0f - o[i]) * r;
        const float lo = (d[i] > 0.0f) ? c : a, hi = (d[i] > 0.0f) ? a : c;
        enter = (lo > enter) ? lo : enter;
        leave = (leave > hi) ? hi : leave;
    }
    leave = (0.0f > leave) ? 0.0f : leave;
    if (enter > leave) return Ones(out);  // the line misses it
    const float ndx = dx * inv, ndy = dy * inv, ndz = dz * inv;
    const float leave1 = (len > leave) ? leave : len;
    const float stop = (leave1 > enter) ? leave1 : enter;
    const float seg = stop - enter;
    const float py = ndy * enter + cy, pz = ndz * enter + cz;
    const float sz = ndz * seg, sy = ndy * seg;
    const float px = ndx * enter + cx;
    const float sx = seg * ndx;
    const float slen = m.sqrt(sy * sy + sx * sx + sz * sz);
    // the height falloff, as for the ellipsoid; then the density (+0x108), without a soft edge
    const float h =
        (py - F(n, 0x150)) * F(n, 0x138) + (px - F(n, 0x14C)) * F(n, 0x134) + (pz - F(n, 0x154)) * F(n, 0x13C);
    float f = m.exp(Clamp80(-h)) * slen;
    const float g = sy * F(n, 0x138) + sx * F(n, 0x134) + sz * F(n, 0x13C);
    if (std::fabs(g) > 0.001f) f = f * ((1.0f - m.exp(Clamp80(-g))) / g);
    Colour(n, m.exp(Clamp80(-(f * F(n, 0x108)))), out);
}

// Cry3DEngine +0x9F630 (with both fog cvars set): each visible fog volume whose box the ray from the camera box's
// centre towards pos hits, blended over the ones before it; then the colour normalised by the coverage, and the
// transmittance
void Trace(const Snapshot& s, const float* cam, const float* pos, float* out, const Math& m) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    const float cx = (s.box[3] + s.box[0]) * 0.5f, cy = (s.box[4] + s.box[1]) * 0.5f, cz = (s.box[5] + s.box[2]) * 0.5f;
    const float ray[6] = {cx, cy, cz, pos[0] - cx, pos[1] - cy, pos[2] - cz};
    for (size_t i = 0; i < s.count; ++i) {
        const uint8_t* n = s.nodes.data() + i * kNodeBytes;
        if (U(n, kFlags) & kHidden) continue;
        float hit[3];
        if (!m.rayBox(ray, reinterpret_cast<const float*>(n + kBounds), hit)) continue;
        float c[4];
        if (U(n, kType) == 0)
            Ellipsoid(n, cam, pos, c, m);
        else
            Box(n, cam, pos, c, m);
        const float a = 1.0f - c[3];
        out[0] = (c[0] - out[0]) * a + out[0];
        out[1] = (c[1] - out[1]) * a + out[1];
        out[2] = (c[2] - out[2]) * a + out[2];
        out[3] = (1.0f - out[3]) * a + out[3];
    }
    const float w = out[3];
    if (w > 0.01f) {
        out[0] = out[0] / w;
        out[1] = out[1] / w;
        out[2] = out[2] / w;
    } else {
        out[0] = out[1] = out[2] = 0.0f;
    }
    out[3] = 1.0f - out[3];
}

bool Locate(char* base, char* trace, Target* out, const char** why) {
    if (!trace || !IsInCode(base, trace, kLoopSearch)) return *why = "TraceFogVolumes not found", false;
    const auto hits = FindInRange(trace, kLoopSearch, ParsePattern(kLoopCode), 2);
    if (hits.size() != 1) return *why = "TraceFogVolumes' loop is not the expected code", false;
    char* loop = hits[0];
    // the camera box's six floats, read one by one: contiguous, the minimum first
    char* minX = Target4(loop + kLoopMinX);
    if (Target4(loop + kLoopMinY) != minX + 4 || Target4(loop + kLoopMinZ) != minX + 8 ||
        Target4(loop + kLoopMaxX) != minX + 12 || Target4(loop + kLoopMaxY) != minX + 16 ||
        Target4(loop + kLoopMaxZ) != minX + 20)
        return *why = "TraceFogVolumes' camera box is not as expected", false;
    char* cvars = Target4(loop + kLoopCvars);
    char* begin = Target4(loop + kLoopBegin);
    char* end = Target4(loop + kLoopEnd);
    if (!IsInImage(base, cvars, 8) || !IsInImage(base, begin, 8) || !IsInImage(base, end, 8) ||
        !IsInImage(base, minX, 24))
        return *why = "TraceFogVolumes' data is outside Cry3DEngine.dll", false;
    char* rayBox = Target4(loop + kLoopRayBox);
    char* ellipsoid = Target4(loop + kLoopEllipsoid);
    char* box = Target4(loop + kLoopBox);
    if (!Code(base, rayBox, kRayBoxCode)) return *why = "the fog's ray-box test is not the expected code", false;
    if (!Code(base, ellipsoid, kEllipsoidCode)) return *why = "the ellipsoid fog is not the expected code", false;
    if (!Code(base, box, kBoxCode)) return *why = "the box fog is not the expected code", false;
    // both read the camera position from the same three floats and call the same sqrtf and expf thunks
    Target t;
    for (int i = 0; i < 3; ++i) {
        char* c = Target4(ellipsoid + kCams[i]);
        if (c != Target4(box + kCams[i]) || !IsInImage(base, c, 4))
            return *why = "the fog's camera position is not as expected", false;
        t.cam[i] = reinterpret_cast<const float*>(c);
    }
    char* sq = Target4(ellipsoid + kEllipsoidSqrt[0]);
    char* ex = Target4(ellipsoid + kEllipsoidExp[0]);
    bool same = sq != ex;
    for (int at : kEllipsoidSqrt) same = same && Target4(ellipsoid + at) == sq;
    for (int at : kEllipsoidExp) same = same && Target4(ellipsoid + at) == ex;
    for (int at : kBoxSqrt) same = same && Target4(box + at) == sq;
    for (int at : kBoxExp) same = same && Target4(box + at) == ex;
    if (!same || !Code(base, sq, kThunk) || !Code(base, ex, kThunk))
        return *why = "the fog's sqrtf and expf are not the expected imports", false;
    t.cvars = reinterpret_cast<char* const*>(cvars);
    t.begin = reinterpret_cast<char* const*>(begin);
    t.end = reinterpret_cast<char* const*>(end);
    t.box = reinterpret_cast<const float*>(minX);
    t.ellipsoid = ellipsoid;
    t.boxFog = box;
    t.math.sqrt = reinterpret_cast<SqrtFn>(sq);
    t.math.exp = reinterpret_cast<ExpFn>(ex);
    t.math.rayBox = reinterpret_cast<RayBoxFn>(rayBox);
    *out = t;
    return true;
}

}  // namespace optiperf::fog
