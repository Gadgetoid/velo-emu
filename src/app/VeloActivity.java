package org.velo_emu.velo;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import java.io.File;
import org.libsdl.app.SDLActivity;

public class VeloActivity extends SDLActivity {
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
}
