/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#include <string>

#include "../GHOST_Types.hh"
#include "GHOST_SystemPaths.hh"

class GHOST_SystemPathsAndroid : public GHOST_SystemPaths {
 public:
  GHOST_SystemPathsAndroid();
  ~GHOST_SystemPathsAndroid() override;

  const char *getSystemDir(int version, const char *versionstr) const override;
  const char *getSystemLibsDir(int version, const char *versionstr) const override;
  const char *getUserDir(int version, const char *versionstr) const override;
  std::optional<std::string> getUserSpecialDir(GHOST_TUserSpecialDirTypes type) const override;
  const char *getBinaryDir() const override;
  void addToSystemRecentFiles(const char *filepath) const override;

 private:
  /* App-private storage roots from ANativeActivity. */
  std::string internal_data_path_;
  std::string external_data_path_;

  /* Stable storage for the const char* return contract. */
  mutable std::string system_dir_;
  mutable std::string system_libs_dir_;
  mutable std::string user_dir_;
};
