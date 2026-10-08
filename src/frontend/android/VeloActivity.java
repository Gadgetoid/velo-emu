package org.velo_emu.velo;

import android.Manifest;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.WindowManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
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

    public boolean savePicture(byte[] png, String name) {
        if (Build.VERSION.SDK_INT < 29) return false;
        ContentValues values = new ContentValues();
        values.put(MediaStore.Images.Media.DISPLAY_NAME, name);
        values.put(MediaStore.Images.Media.MIME_TYPE, "image/png");
        values.put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/Velo");
        Uri uri = getContentResolver().insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values);
        if (uri == null) return false;
        try (OutputStream out = getContentResolver().openOutputStream(uri)) {
            out.write(png);
            return true;
        } catch (Exception e) {
            getContentResolver().delete(uri, null, null);
            return false;
        }
    }

    public boolean sharePicture(byte[] png, String name) {
        File folder = new File(getCacheDir(), VeloShareProvider.FOLDER);
        folder.mkdirs();
        try (FileOutputStream out = new FileOutputStream(new File(folder, name))) {
            out.write(png);
        } catch (Exception e) {
            return false;
        }
        Uri uri = Uri.parse("content://" + getPackageName() + ".share/" + Uri.encode(name));
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("image/png");
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.setClipData(android.content.ClipData.newRawUri(name, uri));
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        Intent chooser = Intent.createChooser(send, null);
        chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(chooser);
        return true;
    }

    public boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    public void requestAllFilesAccess() {
        if (Build.VERSION.SDK_INT >= 30) {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()));
            try {
                startActivity(intent);
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE }, 0);
        }
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
