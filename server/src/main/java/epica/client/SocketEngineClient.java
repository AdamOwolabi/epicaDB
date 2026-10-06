// SocketEngineClient.java -- blocking TCP client for the C++ engine server.
//
// One request is in flight per connection at a time (the methods are
// synchronized), which keeps the framing trivial: write one frame, read one
// frame. The QueryServer gives each connection its own engine client, so
// concurrency comes from multiple connections, not from pipelining.
package epica.client;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public final class SocketEngineClient implements EngineClient {
  private final Socket socket;
  private final DataInputStream in;
  private final OutputStream out;

  private SocketEngineClient(Socket socket) throws IOException {
    this.socket = socket;
    this.in = new DataInputStream(new BufferedInputStream(socket.getInputStream()));
    this.out = new BufferedOutputStream(socket.getOutputStream());
  }

  /** target is "host:port" or just "port". */
  public static SocketEngineClient connect(String target) {
    String host = "127.0.0.1";
    String portStr = target;
    int colon = target.lastIndexOf(':');
    if (colon >= 0) {
      host = target.substring(0, colon);
      portStr = target.substring(colon + 1);
    }
    return connect(host, Integer.parseInt(portStr));
  }

  public static SocketEngineClient connect(String host, int port) {
    try {
      Socket s = new Socket();
      s.setTcpNoDelay(true);
      s.connect(new InetSocketAddress(host, port), 5000);
      return new SocketEngineClient(s);
    } catch (IOException e) {
      throw new EngineException("cannot connect to engine at " + host + ":" + port + ": " + e.getMessage(), e);
    }
  }

  // Sends one payload, returns the response payload (status byte included).
  private synchronized Protocol.Reader roundTrip(byte[] payload) {
    try {
      out.write(new Protocol.Writer().u32(payload.length).bytes());
      out.write(payload);
      out.flush();
      byte[] hdr = new byte[4];
      in.readFully(hdr);
      long len = ByteBuffer.wrap(hdr).order(ByteOrder.LITTLE_ENDIAN).getInt() & 0xffffffffL;
      if (len == 0 || len > Protocol.MAX_FRAME) throw new EngineException("bad frame length " + len);
      byte[] body = new byte[(int) len];
      in.readFully(body);
      return new Protocol.Reader(body);
    } catch (IOException e) {
      throw new EngineException("engine connection failed: " + e.getMessage(), e);
    }
  }

  // Reads the status byte and throws on error statuses.
  private static int status(Protocol.Reader r) {
    int st = r.u8();
    if (st == Protocol.ST_ERROR || st == Protocol.ST_BAD_REQUEST) {
      String msg = new String(r.str(), java.nio.charset.StandardCharsets.UTF_8);
      throw new EngineException((st == Protocol.ST_ERROR ? "engine error: " : "bad request: ") + msg);
    }
    return st;
  }

  public void ping() { status(roundTrip(Protocol.ping())); }

  @Override
  public Optional<byte[]> get(byte[] key) {
    Protocol.Reader r = roundTrip(Protocol.get(key));
    int st = status(r);
    if (st == Protocol.ST_NOT_FOUND) return Optional.empty();
    return Optional.of(r.str());
  }

  @Override
  public void put(byte[] key, byte[] value) { status(roundTrip(Protocol.put(key, value))); }

  @Override
  public void delete(byte[] key) { status(roundTrip(Protocol.del(key))); }

  @Override
  public List<KeyValue> scan(byte[] start, byte[] end, int limit) {
    Protocol.Reader r = roundTrip(Protocol.scan(start, end, limit));
    status(r);
    long n = r.u32();
    List<KeyValue> rows = new ArrayList<>((int) n);
    for (long i = 0; i < n; i++) rows.add(new KeyValue(r.str(), r.str()));
    return rows;
  }

  @Override
  public void batch(List<BatchOp> ops) { status(roundTrip(Protocol.batch(ops))); }

  @Override
  public String stats() {
    Protocol.Reader r = roundTrip(Protocol.stats());
    status(r);
    return new String(r.str(), java.nio.charset.StandardCharsets.UTF_8);
  }

  @Override
  public void close() {
    try {
      socket.close();
    } catch (IOException ignored) {
      // closing is best-effort
    }
  }
}
