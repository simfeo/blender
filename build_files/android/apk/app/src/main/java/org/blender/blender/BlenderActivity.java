/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package org.blender.blender;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.os.Environment;
import android.provider.Settings;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

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

  @Override
  protected void onCreate(Bundle state) {
    /* Runtime files must exist before native Blender init reads them. */
    extractRuntimeIfNeeded();
    setUpPythonInterpreter();
    super.onCreate(state);
    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
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
        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
          nativeOnCommitText(text.toString());
          return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
          nativeOnKey(event.getKeyCode(), event.getAction(), event.getMetaState());
          return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
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
