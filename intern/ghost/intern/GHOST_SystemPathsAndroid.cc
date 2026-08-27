/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#include "GHOST_SystemPathsAndroid.hh"
#include "GHOST_SystemAndroid.hh"

#include <android/native_activity.h>
#include <android_native_app_glue.h>

GHOST_SystemPathsAndroid::GHOST_SystemPathsAndroid()
{
  if (android_app *app = GHOST_SystemAndroid::getAndroidApp()) {
    if (app->activity) {
      if (app->activity->internalDataPath) {
        internal_data_path_ = app->activity->internalDataPath;
      }
      if (app->activity->externalDataPath) {
        external_data_path_ = app->activity->externalDataPath;
      }
    }
  }
}

GHOST_SystemPathsAndroid::~GHOST_SystemPathsAndroid() = default;

const char *GHOST_SystemPathsAndroid::getSystemDir(int /*version*/, const char *versionstr) const
{
  /* Datafiles/scripts are extracted from the APK assets to app-private storage. */
  system_dir_ = internal_data_path_ + "/blender/" + versionstr;
  return system_dir_.c_str();
}

const char *GHOST_SystemPathsAndroid::getSystemLibsDir(int /*version*/,
                                                       const char *versionstr) const
{
  /* Architecture-dependent files live in the same payload as the rest: there is
   * no `/usr/lib` split to mirror here. Without this the base class returns
   * null, `bpy.utils.resource_path('SYSTEM_LIBS')` comes back empty, and the
   * glTF add-on's `dll_path()` then falls through to 'LOCAL' -- which also
   * fails, because it looks beside the binary rather than in the payload. The
   * result is that the meshopt and Draco bridges can never be found, whatever
   * directory they are shipped in. */
  system_libs_dir_ = internal_data_path_ + "/blender/" + versionstr;
  return system_libs_dir_.c_str();
}

const char *GHOST_SystemPathsAndroid::getUserDir(int /*version*/, const char *versionstr) const
{
  user_dir_ = internal_data_path_ + "/config/" + versionstr;
  return user_dir_.c_str();
}

std::optional<std::string> GHOST_SystemPathsAndroid::getUserSpecialDir(
    GHOST_TUserSpecialDirTypes type) const
{
  if (external_data_path_.empty()) {
    return std::nullopt;
  }
  switch (type) {
    case GHOST_kUserSpecialDirDocuments:
      return external_data_path_ + "/Documents";
    case GHOST_kUserSpecialDirDownloads:
      return external_data_path_ + "/Download";
    case GHOST_kUserSpecialDirDesktop:
    case GHOST_kUserSpecialDirCaches:
    case GHOST_kUserSpecialDirPictures:
    case GHOST_kUserSpecialDirVideos:
    case GHOST_kUserSpecialDirMusic:
      return external_data_path_;
    default:
      return std::nullopt;
  }
}

const char *GHOST_SystemPathsAndroid::getBinaryDir() const
{
  return internal_data_path_.c_str();
}

void GHOST_SystemPathsAndroid::addToSystemRecentFiles(const char * /*filepath*/) const {}
