package org.velo_emu.velo;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.view.WindowManager;
import java.io.File;
import org.libsdl.app.SDLActivity;

public class VeloActivity extends SDLActivity {
    private static final int COMMAND_BRIGHTNESS = COMMAND_USER;
    private static final int COMMAND_KEEP_SCREEN_ON = COMMAND_USER + 1;

    private File developmentLibrary() {
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0) return null;
        File library = new File(getFilesDir(), "libmain.so");
        if (!library.isFile()) return null;
        try {
            long installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            return library.lastModified() > installed ? library : null;
        } catch (PackageManager.NameNotFoundException e) {
            return null;
        }
    }

    @Override
    public void loadLibraries() {
        File library = developmentLibrary();
        if (library == null) {
            super.loadLibraries();
            return;
        }
        System.loadLibrary("SDL3");
        System.load(library.getAbsolutePath());
    }

    @Override
    protected String getMainSharedObject() {
        File library = developmentLibrary();
        return library != null ? library.getAbsolutePath() : super.getMainSharedObject();
    }

    public String displayName(String uri) {
        try (Cursor cursor = getContentResolver().query(Uri.parse(uri), new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (Exception e) {
            return null;
        }
        return null;
    }

    @Override
    protected boolean onUnhandledMessage(int command, Object param) {
        int value = param instanceof Integer ? (Integer) param : 0;
        if (command == COMMAND_BRIGHTNESS) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.screenBrightness = value < 0 ? WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE : value / 1000.0f;
            getWindow().setAttributes(attributes);
            return true;
        }
        if (command == COMMAND_KEEP_SCREEN_ON) {
            if (value != 0) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            return true;
        }
        return super.onUnhandledMessage(command, param);
    }
}
