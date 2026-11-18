package org.blender.blender;

import android.app.ActivityManager;
import android.content.Context;
import android.content.res.AssetManager;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * Main Blender Activity, extending SDLActivity.
 *
 * Takes care of extracting the Blender portable version runtime directory from the APK assets,
 * in addition to some SDL specific overrides to load the main Blender shared library and an
 * argument helper.
 */
public class BlenderActivity extends SDLActivity {
    private static final String LOG_TAG = "blender";

    @Override
    protected String[] getLibraries() {
        /* Add libblender.so (main Blender executable). */
        return new String[] {
            "SDL3",
            "blender",
        };
    }

    @Override
    protected String[] getArguments() {
        /* Utility hook to pass command-line start-up arguments via ADB.
         *
         * Usage example:
         *   adb shell am start -n org.blender.blender/.BlenderActivity --es args '"--debug-gpu --log ghost.*,gpu.* --log-level debug"'
         *
         * TODO: Disable for non-debug builds, as otherwise this could lead to injection (any other app can launch
         *       the Blender activity with a --python-expr)
         */

        /* NOTE: naively whitespace-split, quoting (as in --python-expr "something space something") is not supported. */
        List<String> arguments = new ArrayList<>();
        Intent intent = getIntent();

        String extraArguments = intent.getStringExtra("args");
        if (extraArguments != null && !extraArguments.trim().isEmpty()) {
            Collections.addAll(arguments, extraArguments.trim().split("\\s+"));
        }

        if (startupBlendFile != null) {
            arguments.add(startupBlendFile);
        }
        return arguments.toArray(new String[0]);
    }

    /* Resolved in onCreate(), before SDLActivity would forward the intent's raw URI path as a drop event. */
    private String startupBlendFile = null;

    /* Map an incoming .blend to a path Blender can open: the original file when it is reachable in shared
     * storage (so saving and relative paths work), otherwise a private copy. */
    private String blendFileFromIntent(Intent intent) {
        Uri uri = (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) ? intent.getData() : null;
        if (uri == null) {
            return null;
        }
        String path = sharedStoragePathFromUri(uri);
        if (path != null) {
            Log.i(LOG_TAG, "Opening incoming .blend file in place: " + path);
            return path;
        }
        try {
            return copyBlendFileFromUri(uri);
        }
        catch (IOException e) {
            Log.e(LOG_TAG, "Failed to copy blend file from Android file browser", e);
            return null;
        }
    }

    private String sharedStoragePathFromUri(Uri uri) {
        if (!Environment.isExternalStorageManager()) {
            return null;
        }
        if ("file".equals(uri.getScheme())) {
            return readableFile(uri.getPath());
        }
        if (!"content".equals(uri.getScheme())) {
            return null;
        }

        /* Most file managers and MediaStore still expose the legacy `_data` column. */
        try (Cursor cursor = getContentResolver().query(uri, new String[] {"_data"}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                String path = readableFile(cursor.getString(0));
                if (path != null) {
                    return path;
                }
            }
        }
        catch (Exception e) {
            /* Providers without `_data` throw here, fall through to the heuristics below. */
        }

        File root = Environment.getExternalStorageDirectory();
        if (DocumentsContract.isDocumentUri(this, uri)
            && "com.android.externalstorage.documents".equals(uri.getAuthority()))
        {
            String[] parts = DocumentsContract.getDocumentId(uri).split(":", 2);
            if (parts.length == 2 && "primary".equalsIgnoreCase(parts[0])) {
                return readableFile(new File(root, parts[1]).getPath());
            }
        }

        /* FileProvider URIs (e.g. Samsung My Files) embed the storage-relative path after a provider
         * specific prefix: try each suffix of the URI path against the shared storage root. */
        String uriPath = uri.getPath();
        if (uriPath != null) {
            for (int i = uriPath.indexOf('/'); i >= 0; i = uriPath.indexOf('/', i + 1)) {
                String path = readableFile(new File(root, uriPath.substring(i + 1)).getPath());
                if (path != null) {
                    return path;
                }
            }
        }
        return null;
    }

    private static String readableFile(String path) {
        if (path == null) {
            return null;
        }
        File file = new File(path);
        return (file.isFile() && file.canRead()) ? file.getAbsolutePath() : null;
    }

    private int renderScaleDivisor = 1;

    /* Devices under 6GB cannot hold full-resolution targets next to the rest of the system.
     * Override with `adb shell setprop debug.blender.renderdiv N` (1..4). */
    private int computeRenderScaleDivisor() {
        try {
            String value = (String) Class.forName("android.os.SystemProperties")
                .getMethod("get", String.class).invoke(null, "debug.blender.renderdiv");
            if (value != null && !value.isEmpty()) {
                int v = Integer.parseInt(value.trim());
                if (v >= 1 && v <= 4) {
                    return v;
                }
            }
        }
        catch (Exception e) {
            /* No override available, use the memory based default. */
        }
        ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
        ((ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(info);
        return info.totalMem < 6L * 1024 * 1024 * 1024 ? 2 : 1;
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new BlenderSurface(context, renderScaleDivisor);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        String blendFile = blendFileFromIntent(intent);
        if (blendFile != null) {
            SDLActivity.onNativeDropFile(blendFile);
        }
    }

    private String copyBlendFileFromUri(Uri uri) throws IOException {
        File incomingDir = new File(getCacheDir(), "incoming");
        deleteRecursive(incomingDir);
        Files.createDirectories(incomingDir.toPath());

        File destination = Files.createTempFile(incomingDir.toPath(), "blend-", ".blend").toFile();
        try (InputStream source = getContentResolver().openInputStream(uri)) {
            if (source == null) {
                throw new IOException("Failed to open incoming URI: " + uri);
            }
            Files.copy(source, destination.toPath(), StandardCopyOption.REPLACE_EXISTING);
        }

        Log.i(LOG_TAG, "Copied incoming .blend file to " + destination);
        return destination.getAbsolutePath();
    }

    private static void extractAssetDir(AssetManager assets, String path, File targetDir) throws IOException {
        String[] names = assets.list(path);
        if (names == null || names.length == 0) {
            try (InputStream in = assets.open(path)) {
                Files.copy(in, new File(targetDir, path).toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            return;
        }
        File dir = new File(targetDir, path);
        if (!dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("Failed to create " + dir);
        }
        for (String name : names) {
            extractAssetDir(assets, path + "/" + name, targetDir);
        }
    }

    private static void deleteRecursive(File file) throws IOException {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursive(child);
            }
        }
        if (file.exists() && !file.delete()) {
            throw new IOException("Failed to delete " + file);
        }
    }

    /* Ensure the extracted portable version directory is up-to-date and re-extract it if needed. */
    private File extractIfNeeded() throws Exception {
        String extractDirName = "extract";
        String stampFileName = ".extract_stamp";

        File extractDir = new File(getFilesDir(), extractDirName);
        File stampFile = new File(getFilesDir(), stampFileName);

        /* Stamp token: `lastUpdateTime` changes when the app has been updated or re-installed, clean
         * and re-extract in this case. */
        String stampToken = Long.toString(
            getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);

        if (extractDir.isDirectory()
            && stampFile.isFile()
            && stampToken.equals(new String(Files.readAllBytes(stampFile.toPath()), StandardCharsets.UTF_8))) {
            return extractDir;
        }

        Log.i(LOG_TAG, "Portable version directory missing or outdated, clearing and extracting...");

        /* Fully clean rather than overwrite to ensure files dropped since the previous version do not
         * linger in the extracted tree. */
        deleteRecursive(extractDir);
        extractAssetDir(getAssets(), extractDirName, getFilesDir());

        /* Written last to ensure an interrupted extraction leaves no stamp and re-runs on the next launch. */
        Files.write(stampFile.toPath(), stampToken.getBytes(StandardCharsets.UTF_8));

        Log.i(LOG_TAG, "Portable version directory extracted to " + extractDir);
        return extractDir;
    }

    private void hideNavigationBar() {
        /* Use the same system UI flags as SDL, but keep the status bar visible. */
        View decorView = getWindow().getDecorView();
        int flags = decorView.getSystemUiVisibility();
        flags &= ~(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                 View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                 View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                 View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        decorView.setSystemUiVisibility(flags);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideNavigationBar();
        }
    }

    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        /* Replace SDL's callback, which hides the status bar in fullscreen mode. */
        if (hasWindowFocus()) {
            hideNavigationBar();
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            File extractDir = extractIfNeeded();

            /* Pass the extractDir path via an environment variable as getFilesDir() isn't directly
             * exposed by the NDK. Also allows defining its paths in less places. Temporary, shall be
             * replaced by a NativeActivity property access, or direct access to the APK assets which
             * would remove the need for extraction altogether. */
            Os.setenv("BLENDER_ANDROID_EXTRACT_DIR", extractDir.getAbsolutePath(), true);

            /* User home and temp directories, emulated from Linux and fed via environment variables.
             * For now, give a custom sub-folder for $HOME, without which the user drops into the app
             * files root (getFilesDir()).
             * TODO: Replace with a proper Blender-side Android implementation. */
            File homeDir = new File(getFilesDir(), "home");
            homeDir.mkdir();
            Os.setenv("HOME", homeDir.getAbsolutePath(), true);
            Os.setenv("TMPDIR", getCacheDir().getAbsolutePath(), true);

            /* GHOST scales its DPI hint by this so the UI keeps its physical size. */
            renderScaleDivisor = computeRenderScaleDivisor();
            Os.setenv("BLENDER_ANDROID_RENDER_DIVISOR", Integer.toString(renderScaleDivisor), true);
            Log.i(LOG_TAG, "Render scale divisor: " + renderScaleDivisor);
        } catch (Exception e) {
            throw new RuntimeException("Failed to extract Blender runtime files", e);
        }

        /* SDLActivity.onCreate() forwards any intent data path as a drop event, which for content URIs
         * is not a file path. The file is opened through the arguments instead. */
        startupBlendFile = blendFileFromIntent(getIntent());
        if (getIntent() != null && getIntent().getData() != null) {
            setIntent(new Intent(getIntent()).setData(null));
        }

        super.onCreate(savedInstanceState);
        requestAllFilesAccessIfNeeded();
    }

    private void requestAllFilesAccessIfNeeded() {
        if (Environment.isExternalStorageManager()) {
            return;
        }
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
        }
        catch (Exception e) {
            Log.w(LOG_TAG, "Failed to open the all-files access settings", e);
        }
    }
}
