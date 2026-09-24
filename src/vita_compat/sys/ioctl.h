#pragma once

// PS Vita has no sys/ioctl.h. SQLite includes it on Unix even when the
// locking styles which use ioctl are disabled.
