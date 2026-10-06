// Bytes.java -- byte[] helpers: UTF-8 conversion, printable rendering, the
// prefix-successor computation, and "contains".
package epica.query;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class Bytes {
  private Bytes() {}

  public static byte[] utf8(String s) { return s.getBytes(StandardCharsets.UTF_8); }

  public static String str(byte[] b) { return new String(b, StandardCharsets.UTF_8); }

  /** Renders bytes for humans: quoted, with control bytes escaped. */
  public static String show(byte[] b) {
    StringBuilder sb = new StringBuilder("\"");
    for (byte x : b) {
      int c = x & 0xff;
      if (c == '"') sb.append("\\\"");
      else if (c == '\\') sb.append("\\\\");
      else if (c == '\n') sb.append("\\n");
      else if (c == '\t') sb.append("\\t");
      else if (c < 0x20 || c == 0x7f) sb.append(String.format("\\x%02x", c));
      else sb.append((char) c);
    }
    return sb.append('"').toString();
  }

  /**
   * Smallest byte string greater than every string with the given prefix, or
   * empty (= unbounded) if none exists (prefix is all 0xFF bytes).
   *
   *   "user:"  -> "user;"      ("user:" + 1 in the last byte)
   *   "ab\xff" -> "ac"         (carry: drop trailing 0xFF, increment "b")
   *   "\xff"   -> ""           (nothing is greater: scan to the end)
   */
  public static byte[] prefixSuccessor(byte[] prefix) {
    byte[] out = Arrays.copyOf(prefix, prefix.length);
    for (int i = out.length - 1; i >= 0; i--) {
      if ((out[i] & 0xff) != 0xff) {
        out[i]++;
        return Arrays.copyOf(out, i + 1);
      }
    }
    return new byte[0];
  }

  /** Smallest byte string strictly greater than key: key + 0x00. Used for pagination. */
  public static byte[] immediateSuccessor(byte[] key) {
    byte[] out = Arrays.copyOf(key, key.length + 1);
    out[key.length] = 0;
    return out;
  }

  public static boolean contains(byte[] haystack, byte[] needle) {
    if (needle.length == 0) return true;
    outer:
    for (int i = 0; i + needle.length <= haystack.length; i++) {
      for (int j = 0; j < needle.length; j++) {
        if (haystack[i + j] != needle[j]) continue outer;
      }
      return true;
    }
    return false;
  }
}
