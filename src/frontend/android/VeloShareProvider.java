package org.velo_emu.velo;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

public class VeloShareProvider extends ContentProvider {
    static final String FOLDER = "share";

    private File fileFor(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.contains("/")) throw new FileNotFoundException();
        File file = new File(new File(getContext().getCacheDir(), FOLDER), name);
        if (!file.isFile()) throw new FileNotFoundException();
        return file;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] arguments, String order) {
        MatrixCursor cursor = new MatrixCursor(new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE });
        try {
            File file = fileFor(uri);
            cursor.addRow(new Object[] { file.getName(), file.length() });
        } catch (FileNotFoundException e) {
            return cursor;
        }
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        return "image/png";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] arguments) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] arguments) {
        return 0;
    }
}
