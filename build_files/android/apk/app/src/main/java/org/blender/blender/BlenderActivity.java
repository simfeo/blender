/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package org.blender.blender;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.system.Os;
import android.util.Log;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * NativeActivity subclass for Blender. Extracts the bundled runtime
 * (Python + scripts + datafiles) on first launch, then loads libblender.so
 * and bridges soft-keyboard IME text to native. Landscape only.
 */
public class BlenderActivity extends NativeActivity {

  /* NativeActivity dlopen()s the library from native code, which never registers
   * it with the class loader, so the JNI lookup for the native methods below
   * fails with UnsatisfiedLinkError. Load it here as well to register it. */
  static {
    System.loadLibrary("blender");
  }

  /* Must match GHOST_SystemPathsAndroid: <filesDir>/blender/<version>. */
  private static final String VERSION = "5.3";
  private static final String RUNTIME_ZIP = "blender_runtime.zip";
  private static final String PYTHON_VERSION = "3.13";
  private static final String PYTHON_FULL_VERSION = "3.13.13";
  private static final String PYTHON_BIN_LIB = "libpython3_13_bin.so";
  private static final String TAG = "Blender";

  private InputView inputView;

  private native void nativeOnCommitText(String text);
  private native void nativeOnKey(int keycode, int action, int metaState);
  private native void nativeSetLogPath(String path);
  private native void nativeOpenMainFile(String path);

  private String logPath = null;

  @Override
  protected void onCreate(Bundle state) {
    /* Runtime files must exist before native Blender init reads them. */
    extractRuntimeIfNeeded();
    setUpPythonInterpreter();
    /* Before super.onCreate: that is what starts the native activity, and the
     * log is opened as the first thing it does. */
    chooseLogPath();
    publishHardwareNames();
    /* Also before super.onCreate: the native side reads it while building argv. */
    publishLaunchFile(getIntent());
    super.onCreate(state);
    announceLogPath();
    /* Sensor variant, so the tablet can be picked up from either side. Plain
     * LANDSCAPE names one direction, which leaves the app upside down after a
     * 180 degree turn. Portrait stays excluded either way. */
    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    enterImmersive();
    requestAllFilesAccess();

    inputView = new InputView(this);
    addContentView(inputView, new ViewGroup.LayoutParams(1, 1));
  }

  /* Scoped storage confines the app to its sandbox, but Blender opens and saves
   * .blend files and their assets anywhere by path. Send the user to the "All
   * files access" screen once; it is a no-op after they grant it. */
  private void requestAllFilesAccess() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()) {
      return;
    }
    try {
      Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                 Uri.parse("package:" + getPackageName()));
      startActivity(intent);
    }
    catch (Exception ex) {
      /* Some devices lack the per-app screen; fall back to the global list. */
      try {
        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
      }
      catch (Exception ignored) {
      }
    }
  }

  /* Device and chip names for the Preferences panel. The Linux side cannot read them:
   * arm64 /proc/cpuinfo has no model name and the sysfs paths are closed to apps.
   * Anything missing stays unset, so the panel leaves the line out. */
  private void publishHardwareNames() {
    try {
      /* Build.MANUFACTURER is lower case ("samsung"). */
      String device = joinNonEmpty(capitalize(Build.MANUFACTURER), Build.MODEL);
      if (!device.isEmpty()) {
        Os.setenv("BLENDER_ANDROID_DEVICE", device, true);
      }
      /* Qualcomm parts report "QTI", which nobody recognises. */
      String vendor = Build.SOC_MANUFACTURER;
      if ("QTI".equalsIgnoreCase(vendor)) {
        vendor = "Qualcomm";
      }
      String soc = joinNonEmpty(vendor, Build.SOC_MODEL);
      if (!soc.isEmpty()) {
        Os.setenv("BLENDER_ANDROID_SOC", soc, true);
      }
    }
    catch (Exception ex) {
      Log.w(TAG, "hardware names unavailable", ex);
    }
  }

  private static String capitalize(String text) {
    if (text == null || text.isEmpty()) {
      return text;
    }
    return Character.toUpperCase(text.charAt(0)) + text.substring(1);
  }

  /* Unset Build fields read "unknown", which would be printed. */
  private static String joinNonEmpty(String a, String b) {
    StringBuilder out = new StringBuilder();
    for (String part : new String[] {a, b}) {
      if (part == null) {
        continue;
      }
      part = part.trim();
      if (part.isEmpty() || part.equalsIgnoreCase(Build.UNKNOWN)) {
        continue;
      }
      if (out.length() != 0) {
        out.append(' ');
      }
      out.append(part);
    }
    return out.toString();
  }

  /* A .blend tapped in a file manager while Blender is running. Queued in GHOST as
   * GHOST_kEventOpenMainFile, which wm_window.cc answers with WM_OT_open_mainfile, so
   * the unsaved-changes prompt and recent files behave as they do from the File menu. */
  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    String path = resolveBlendPath(intent);
    if (path != null) {
      nativeOpenMainFile(path);
    }
  }

  /* Cold start: the file goes in as a launch argument, the way a double-clicked file
   * reaches argv on macOS, so it loads instead of the startup file rather than
   * replacing it afterwards. The native thread has not started yet, hence the
   * environment. */
  private void publishLaunchFile(Intent intent) {
    String path = resolveBlendPath(intent);
    if (path == null) {
      return;
    }
    try {
      Os.setenv("BLENDER_ANDROID_OPEN_FILE", path, true);
    }
    catch (Exception ex) {
      Log.w(TAG, "cannot publish launch file", ex);
    }
  }

  private String resolveBlendPath(Intent intent) {
    if (intent == null) {
      return null;
    }
    String action = intent.getAction();
    if (!Intent.ACTION_VIEW.equals(action) && !Intent.ACTION_EDIT.equals(action)) {
      return null;
    }
    Uri uri = intent.getData();
    if (uri == null) {
      return null;
    }
    try {
      String path = resolveUriToPath(uri);
      Log.i(TAG, "open request " + uri + " -> " + path);
      return path;
    }
    catch (Exception ex) {
      Log.w(TAG, "cannot resolve " + uri, ex);
      return null;
    }
  }

  /* Blender opens files by path, and a .blend opened from a copy loses the relative
   * paths to its textures and libraries and saves back where nobody will find it.
   * So each step tries to name the real file; the copy is the last resort. */
  private String resolveUriToPath(Uri uri) throws Exception {
    if ("file".equals(uri.getScheme())) {
      String path = usable(uri.getPath());
      if (path != null) {
        return path;
      }
    }

    /* The storage document provider spells volume and relative path into the id:
     * "primary:Download/scene.blend". */
    if (DocumentsContract.isDocumentUri(this, uri) &&
        "com.android.externalstorage.documents".equals(uri.getAuthority()))
    {
      String[] id = DocumentsContract.getDocumentId(uri).split(":", 2);
      if (id.length == 2) {
        File root = "primary".equalsIgnoreCase(id[0]) ? Environment.getExternalStorageDirectory() :
                                                        volumeRoot(id[0]);
        if (root != null) {
          String path = usable(new File(root, id[1]).getAbsolutePath());
          if (path != null) {
            return path;
          }
        }
      }
    }

    /* MediaStore's DATA column is deprecated but still carries the real path. */
    if ("content".equals(uri.getScheme())) {
      try (Cursor c = getContentResolver().query(
               uri, new String[] {MediaStore.MediaColumns.DATA}, null, null, null))
      {
        if (c != null && c.moveToFirst() && !c.isNull(0)) {
          String path = usable(c.getString(0));
          if (path != null) {
            return path;
          }
        }
      }
      catch (Exception ignored) {
        /* A provider is free to reject the column. */
      }
    }

    /* For any other provider the descriptor is usually a real file, which
     * /proc/self/fd names. */
    try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
      if (pfd != null) {
        String path = usable(Os.readlink("/proc/self/fd/" + pfd.getFd()));
        if (path != null) {
          return path;
        }
      }
    }
    catch (Exception ignored) {
    }

    /* Nothing real behind it, or all-files access is not granted yet (it is only
     * requested after onCreate). A copy still opens; relative links will not resolve. */
    return copyToCache(uri);
  }

  /* Only accept a path this process can actually open, so a missing permission falls
   * through to the copy instead of ending in an error. */
  private static String usable(String path) {
    if (path == null) {
      return null;
    }
    File file = new File(path);
    return (file.isFile() && file.canRead()) ? file.getAbsolutePath() : null;
  }

  private File volumeRoot(String uuid) {
    try {
      StorageManager sm = (StorageManager)getSystemService(Context.STORAGE_SERVICE);
      for (StorageVolume volume : sm.getStorageVolumes()) {
        if (uuid.equalsIgnoreCase(volume.getUuid())) {
          return volume.getDirectory();
        }
      }
    }
    catch (Exception ignored) {
    }
    return null;
  }

  private String copyToCache(Uri uri) throws Exception {
    File dir = new File(getCacheDir(), "opened");
    dir.mkdirs();
    File out = new File(dir, displayName(uri));
    try (InputStream is = getContentResolver().openInputStream(uri);
         OutputStream os = new FileOutputStream(out))
    {
      if (is == null) {
        return null;
      }
      byte[] buf = new byte[65536];
      int n;
      while ((n = is.read(buf)) > 0) {
        os.write(buf, 0, n);
      }
    }
    Log.w(TAG, "no path behind " + uri + "; opening a copy, relative links will not resolve");
    return out.getAbsolutePath();
  }

  /* A provider-supplied name is untrusted and may carry separators or "..". */
  private String displayName(Uri uri) {
    String name = null;
    try (Cursor c = getContentResolver().query(
             uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null))
    {
      if (c != null && c.moveToFirst() && !c.isNull(0)) {
        name = c.getString(0);
      }
    }
    catch (Exception ignored) {
    }
    if (name == null || name.isEmpty()) {
      name = "opened.blend";
    }
    name = new File(name).getName().replace("..", "_");
    return name.toLowerCase().endsWith(".blend") ? name : name + ".blend";
  }

  /* Called from native (GHOST_android_open_url). Every link in Blender ends at
   * Python's webbrowser, which finds no browser on Android and fails silently.
   * NEW_TASK because the browser belongs in its own task and the call does not come
   * through an Activity context. */
  public boolean openUrl(String url) {
    try {
      Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
      intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
      startActivity(intent);
      return true;
    }
    catch (Exception ex) {
      /* No browser installed, or one refused the intent. */
      Log.w(TAG, "cannot open " + url, ex);
      return false;
    }
  }

  /* Hide the status/navigation bars so they don't overlap Blender's own menus
   * (the top File/Edit/… bar and the bottom timeline). Sticky immersive lets the
   * user swipe from an edge to reveal the bars temporarily. */
  private void enterImmersive() {
    View d = getWindow().getDecorView();
    d.setSystemUiVisibility(
        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_FULLSCREEN
        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    /* Immersive mode is cleared when focus returns (e.g. after the soft keyboard
     * or a system dialog); re-apply it. */
    if (hasFocus) {
      enterImmersive();
    }
  }

  /* Blender runs the extension command line tool as a subprocess, so
   * sys.executable has to be real. The interpreter ships in the native library
   * directory because the runtime payload lands in the app's data directory,
   * which is mounted noexec from API 29 on, and Blender looks for it under
   * <python>/bin, so the two are linked here.
   *
   * Redone on every launch: nativeLibraryDir carries a hash that changes when
   * the app is updated, which would leave the link dangling. */
  private void setUpPythonInterpreter() {
    File pythonHome = new File(getFilesDir(), "blender/" + VERSION + "/python");
    if (!pythonHome.isDirectory()) {
      return;
    }
    try {
      File interpreter = new File(getApplicationInfo().nativeLibraryDir, PYTHON_BIN_LIB);
      if (!interpreter.isFile()) {
        return;
      }
      File binDir = new File(pythonHome, "bin");
      binDir.mkdirs();
      File link = new File(binDir, "python" + PYTHON_VERSION);
      /* delete() rather than exists(): a dangling symlink reads as absent but
       * still makes symlink() fail with EEXIST. */
      link.delete();
      Os.symlink(interpreter.getAbsolutePath(), link.getAbsolutePath());

      /* A child process gets none of Blender's Python configuration, so it
       * would compute its prefix from the interpreter's own location, the
       * library directory, which holds no standard library. Blender's embedded
       * interpreter is unaffected: it runs an isolated config that ignores
       * PYTHONHOME and sets its home explicitly. */
      Os.setenv("PYTHONHOME", pythonHome.getAbsolutePath(), true);

      /* PYTHONHOME alone is not enough. The extension system passes
       * bpy.app.python_args, which is ("-I",), so the child starts isolated:
       * that implies -E and therefore ignores PYTHONHOME. It then resolves the
       * symlink back to the library directory and dies with "Failed to import
       * encodings module" before running a line.
       *
       * pyvenv.cfg is the way out. CPython reads it beside the executable or
       * one level up and -E does not suppress it, which is exactly how a
       * virtualenv's symlinked interpreter finds its base. The path is only
       * known at runtime, so it is written here rather than shipped. */
      writeText(new File(pythonHome, "pyvenv.cfg"),
          "home = " + binDir.getAbsolutePath() + "\n"
              + "include-system-site-packages = true\n"
              + "version = " + PYTHON_FULL_VERSION + "\n");

      /* A child is a plain exec outside the app's linker namespace, so it
       * resolves "libcrypto.so" against the system paths and finds Android's
       * BoringSSL, which does not export what the bundled _ssl module needs.
       * Naming the library directory first puts the real OpenSSL ahead of it.
       * The app's own libraries are already loaded by this point, so this only
       * affects the extension system's subprocesses. */
      Os.setenv("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir, true);

      /* Blender sets this itself only for a portable install, which an APK is
       * not, so OpenSSL would look for certificates in the path it was
       * configured with on the build machine. */
      File certs = new File(pythonHome,
          "lib/python" + PYTHON_VERSION + "/site-packages/certifi/cacert.pem");
      if (certs.isFile()) {
        Os.setenv("SSL_CERT_FILE", certs.getAbsolutePath(), true);
      }
    }
    catch (Exception ex) {
      /* Not fatal: everything except online extensions works without it. */
      Log.w(TAG, "python interpreter setup failed", ex);
    }
  }

  private static String readText(File f) {
    InputStream in = null;
    try {
      in = new java.io.FileInputStream(f);
      byte[] buffer = new byte[64];
      int n = in.read(buffer);
      return n > 0 ? new String(buffer, 0, n, "UTF-8").trim() : "";
    }
    catch (Exception ex) {
      return "";
    }
    finally {
      if (in != null) {
        try {
          in.close();
        }
        catch (Exception ignored) {
        }
      }
    }
  }

  private static void deleteTree(File f) {
    File[] children = f.listFiles();
    if (children != null) {
      for (File child : children) {
        deleteTree(child);
      }
    }
    f.delete();
  }

  private static void writeText(File f, String text) {
    OutputStream out = null;
    try {
      out = new FileOutputStream(f);
      out.write(text.getBytes("UTF-8"));
    }
    catch (Exception ex) {
      Log.w(TAG, "could not write " + f, ex);
    }
    finally {
      if (out != null) {
        try {
          out.close();
        }
        catch (Exception ignored) {
        }
      }
    }
  }

  /* Everything Blender prints is copied to this file, because logcat needs adb
   * and someone reporting a failure usually cannot run it. Picked here rather
   * than in native code so the directory is created and tested for real, and so
   * the user can be told where to find it. */
  private void chooseLogPath() {
    File[] candidates = {
        Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
        Environment.getExternalStorageDirectory(),
        getExternalFilesDir(null),
    };
    for (File dir : candidates) {
      if (dir == null) {
        continue;
      }
      /* Existence is not enough: shared storage is unwritable until the
       * all-files permission is granted, and that fails at open, not here. */
      dir.mkdirs();
      File candidate = new File(dir, "blender-startup.log");
      try (FileOutputStream probe = new FileOutputStream(candidate)) {
        logPath = candidate.getAbsolutePath();
        break;
      }
      catch (Exception ignored) {
        /* Try the next one. */
      }
    }
    if (logPath != null) {
      nativeSetLogPath(logPath);
    }
  }

  private void announceLogPath() {
    final String message = (logPath != null) ? "Log: " + logPath :
                                               "No writable location for the log";
    runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_LONG).show());
  }

  private void extractRuntimeIfNeeded() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    File marker = new File(root, ".installed-" + VERSION);

    /* Keyed on the install time rather than the version: the payload changes
     * with every build during development, and a marker that only records the
     * version leaves the previous runtime in place, so a reinstalled APK runs
     * yesterday's scripts against today's binary. */
    long stamp;
    try {
      stamp = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
    }
    catch (Exception ex) {
      stamp = 0;
    }
    String want = Long.toString(stamp);

    if (marker.isFile() && want.equals(readText(marker))) {
      return;
    }
    deleteTree(root);
    root.mkdirs();
    try (InputStream is = getAssets().open(RUNTIME_ZIP, AssetManager.ACCESS_STREAMING);
         ZipInputStream zis = new ZipInputStream(is)) {
      ZipEntry e;
      byte[] buf = new byte[65536];
      while ((e = zis.getNextEntry()) != null) {
        File out = new File(root, e.getName());
        if (e.isDirectory()) {
          out.mkdirs();
          continue;
        }
        File parent = out.getParentFile();
        if (parent != null) {
          parent.mkdirs();
        }
        try (OutputStream os = new FileOutputStream(out)) {
          int n;
          while ((n = zis.read(buf)) > 0) {
            os.write(buf, 0, n);
          }
        }
      }
      writeText(marker, want);
    }
    catch (Exception ex) {
      throw new RuntimeException("Failed to extract Blender runtime", ex);
    }
  }

  /* Called from native (popupOnScreenKeyboard). */
  public void showKeyboard() {
    runOnUiThread(() -> {
      inputView.setFocusableInTouchMode(true);
      inputView.requestFocus();
      InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
      imm.showSoftInput(inputView, InputMethodManager.SHOW_IMPLICIT);
    });
  }

  /* Called from native (hideOnScreenKeyboard). */
  public void hideKeyboard() {
    runOnUiThread(() -> {
      InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
      imm.hideSoftInputFromWindow(inputView.getWindowToken(), 0);
    });
  }

  /** Invisible view whose InputConnection captures IME text. */
  private class InputView extends View {
    InputView(Context context) {
      super(context);
      setFocusable(true);
      setFocusableInTouchMode(true);
    }

    @Override
    public boolean onCheckIsTextEditor() {
      return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
      outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
      outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;

      return new BaseInputConnection(this, false) {
        /* The word the IME is still composing. Soft keyboards send no key events for
         * ordinary characters: they call setComposingText() per keystroke with the
         * whole word so far and commitText() only when it is finished. Blender has no
         * composing text, so the word is mirrored into the field as it changes;
         * without this, letters only arrive when a suggestion is picked. */
        private String composing = "";

        /** Makes the field show `text` where it currently shows `composing`. */
        private void replaceComposing(String text) {
          int common = 0;
          final int max = Math.min(composing.length(), text.length());
          while (common < max && composing.charAt(common) == text.charAt(common)) {
            common++;
          }
          /* Never split a surrogate pair: half of one is not valid UTF-8 in GHOST. */
          if (common > 0 && common < text.length() &&
              Character.isLowSurrogate(text.charAt(common)))
          {
            common--;
          }
          /* One delete on Blender's side removes one code point. */
          final int stale = composing.codePointCount(common, composing.length());
          for (int i = 0; i < stale; i++) {
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_DOWN, 0);
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_UP, 0);
          }
          if (common < text.length()) {
            nativeOnCommitText(text.substring(common));
          }
          composing = text;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
          replaceComposing(text.toString());
          return true;
        }

        @Override
        public boolean finishComposingText() {
          /* Already in the field as typed. */
          composing = "";
          return true;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
          /* Usually the composition unchanged, but autocorrect and suggestions commit
           * something different; diffing covers both. */
          replaceComposing(text.toString());
          composing = "";
          return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
          composing = "";
          nativeOnKey(event.getKeyCode(), event.getAction(), event.getMetaState());
          return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
          composing = "";
          for (int i = 0; i < beforeLength; i++) {
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_DOWN, 0);
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_UP, 0);
          }
          return true;
        }
      };
    }
  }
}
