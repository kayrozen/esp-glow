#ifndef LFS_SHIM_INTTYPES_H
#define LFS_SHIM_INTTYPES_H
/* lfs_util.h only pulls this in for PRI* macros used in LFS_TRACE/DEBUG/
   WARN/ERROR format strings; the shim disables all of those (LFS_NO_DEBUG,
   LFS_NO_WARN, LFS_NO_ERROR, no LFS_YES_TRACE), so nothing here needs to be
   real. */
#endif
