// Protocol.java -- Java side of the binary wire format defined in
// engine/net/protocol.h. Byte-for-byte identical framing: little-endian
// u32 length prefix, u8 opcode/status, length-prefixed byte strings.
//
// Java's ByteBuffer defaults to big-endian, so every buffer here is
// explicitly switched to LITTLE_ENDIAN -- a classic source of cross-language
// bugs and a good thing to be able to explain.
package epica.client;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class Protocol {
  public static final byte OP_PING = 0x01;
  public static final byte OP_GET = 0x02;
  public static final byte OP_PUT = 0x03;
  public static final byte OP_DEL = 0x04;
  public static final byte OP_SCAN = 0x05;
  public static final byte OP_BATCH = 0x06;
  public static final byte OP_STATS = 0x07;

  public static final byte ST_OK = 0;
  public static final byte ST_NOT_FOUND = 1;
  public static final byte ST_ERROR = 2;
  public static final byte ST_BAD_REQUEST = 3;

  public static final int MAX_FRAME = 64 * 1024 * 1024;

  private Protocol() {}

  /** Builds a request/response payload. */
  public static final class Writer {
    private final ByteArrayOutputStream out = new ByteArrayOutputStream();

    public Writer u8(int v) {
      out.write(v & 0xff);
      return this;
    }

    public Writer u32(long v) {
      out.write((int) (v & 0xff));
      out.write((int) ((v >>> 8) & 0xff));
      out.write((int) ((v >>> 16) & 0xff));
      out.write((int) ((v >>> 24) & 0xff));
      return this;
    }

    public Writer str(byte[] b) {
      u32(b.length);
      out.write(b, 0, b.length);
      return this;
    }

    public byte[] bytes() { return out.toByteArray(); }

    /** Payload wrapped with the u32 length prefix: what actually goes on the socket. */
    public byte[] frame() {
      byte[] payload = bytes();
      return new Writer().u32(payload.length).bytes(payload).bytes();
    }

    private Writer bytes(byte[] b) {
      out.write(b, 0, b.length);
      return this;
    }
  }

  /** Parses a payload. Throws EngineException on truncation. */
  public static final class Reader {
    private final ByteBuffer buf;

    public Reader(byte[] payload) {
      this.buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
    }

    public int u8() {
      need(1);
      return buf.get() & 0xff;
    }

    public long u32() {
      need(4);
      return buf.getInt() & 0xffffffffL;
    }

    public byte[] str() {
      long n = u32();
      if (n > buf.remaining()) throw new EngineException("truncated string in response");
      byte[] b = new byte[(int) n];
      buf.get(b);
      return b;
    }

    public boolean hasRemaining() { return buf.hasRemaining(); }

    private void need(int n) {
      if (buf.remaining() < n) throw new EngineException("truncated response");
    }
  }

  // --- request encoders (payloads, not yet framed) ---
  public static byte[] ping() { return new Writer().u8(OP_PING).bytes(); }
  public static byte[] stats() { return new Writer().u8(OP_STATS).bytes(); }
  public static byte[] get(byte[] key) { return new Writer().u8(OP_GET).str(key).bytes(); }
  public static byte[] del(byte[] key) { return new Writer().u8(OP_DEL).str(key).bytes(); }
  public static byte[] put(byte[] key, byte[] value) {
    return new Writer().u8(OP_PUT).str(key).str(value).bytes();
  }
  public static byte[] scan(byte[] start, byte[] end, int limit) {
    return new Writer().u8(OP_SCAN).str(start).str(end).u32(limit).bytes();
  }
  public static byte[] batch(java.util.List<BatchOp> ops) {
    Writer w = new Writer().u8(OP_BATCH).u32(ops.size());
    for (BatchOp op : ops) {
      w.u8(op.isPut() ? 1 : 0).str(op.key());
      if (op.isPut()) w.str(op.value());
    }
    return w.bytes();
  }
}
