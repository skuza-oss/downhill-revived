// Downhill Revived on Android: the shared log's file for the app the player shares it with (finding
// 152). Our own, in place of androidx's FileProvider (no androidx in this app): not exported, so an
// app reads a file only through the read grant of the share it was handed; read-only; one folder
// (LogShare's, in the cache), plain file names only. It answers OpenableColumns' name and size, which
// mail and chat apps ask for to name the attachment.
package app.downhillrevived;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.List;

public class LogProvider extends ContentProvider {
    static final String AUTHORITY = "app.downhillrevived.logs";

    static Uri uri(File file) {
        return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(file.getName()).build();
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    /** The file uri names: one plain name in LogShare's folder, which exists. */
    private File file(Uri uri) throws FileNotFoundException {
        List<String> path = uri.getPathSegments();
        String name = path.size() == 1 ? path.get(0) : "";
        if (name.isEmpty() || name.startsWith(".") || name.contains("/") || name.contains("\\"))
            throw new FileNotFoundException(uri.toString());
        File f = new File(LogShare.folder(getContext()), name);
        if (!f.isFile()) throw new FileNotFoundException(uri.toString());
        return f;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read only");
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File f;
        try {
            f = file(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] columns = projection != null ? projection : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(columns[i])) row[i] = f.length();
        }
        MatrixCursor c = new MatrixCursor(columns, 1);
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read only");
    }
}
