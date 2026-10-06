// QueryServer.java -- a line-oriented TCP front end for the query language.
//
// Any client that can send text lines (nc, telnet, a script) can use it:
//
//   $ printf 'PUT a 1\nGET a\nSCAN\n' | nc 127.0.0.1 7380
//   OK
//   VALUE "1"
//   ROW a "1"
//   END 1
//
// Response grammar (one response per request line):
//   OK | NOT_FOUND | VALUE <shown> | COUNT <n> | ERROR <message>
//   ROW <key> <shown-value> ... END <n>          for scans
//   TEXT <n-lines> followed by n lines            for STATS / EXPLAIN
//
// Concurrency: thread per client connection, each with its own engine
// connection (an EngineClient is single-request-at-a-time). The C++ engine
// handles the real concurrency; this layer just keeps connections
// independent so one slow client cannot block another.
package epica.server;

import epica.client.EngineClient;
import epica.client.EngineException;
import epica.client.KeyValue;
import epica.query.Bytes;
import epica.query.Executor;
import epica.query.QueryException;
import epica.query.QueryResult;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Supplier;

public final class QueryServer {
  private final int requestedPort;
  private final Supplier<EngineClient> engineFactory;
  private ServerSocket listener;
  private Thread acceptThread;
  private final AtomicBoolean stopping = new AtomicBoolean(false);
  private final AtomicLong requests = new AtomicLong();

  /** engineFactory creates one engine connection per client connection. */
  public QueryServer(int port, Supplier<EngineClient> engineFactory) {
    this.requestedPort = port;
    this.engineFactory = engineFactory;
  }

  public void start() throws IOException {
    listener = new ServerSocket(requestedPort);
    acceptThread = new Thread(this::acceptLoop, "epica-query-accept");
    acceptThread.start();
  }

  public int port() { return listener.getLocalPort(); }

  public long requestsServed() { return requests.get(); }

  public void join() throws InterruptedException { acceptThread.join(); }

  public void stop() {
    stopping.set(true);
    try {
      listener.close();  // unblocks accept()
    } catch (IOException ignored) {
      // best effort
    }
  }

  private void acceptLoop() {
    while (!stopping.get()) {
      try {
        Socket s = listener.accept();
        Thread t = new Thread(() -> handle(s), "epica-query-conn");
        t.setDaemon(true);
        t.start();
      } catch (SocketException e) {
        break;  // listener closed
      } catch (IOException e) {
        if (!stopping.get()) System.err.println("accept failed: " + e.getMessage());
      }
    }
  }

  private void handle(Socket socket) {
    try (socket;
         EngineClient engine = engineFactory.get();
         BufferedReader in = new BufferedReader(new InputStreamReader(socket.getInputStream(), StandardCharsets.UTF_8));
         PrintWriter out = new PrintWriter(socket.getOutputStream(), false, StandardCharsets.UTF_8)) {
      Executor executor = new Executor(engine);
      String line;
      while ((line = in.readLine()) != null) {
        line = line.trim();
        if (line.isEmpty()) continue;
        if (line.equalsIgnoreCase("quit")) break;
        requests.incrementAndGet();
        try {
          render(executor.execute(line), out);
        } catch (QueryException e) {
          out.println("ERROR syntax: " + e.getMessage());
        } catch (EngineException e) {
          out.println("ERROR engine: " + e.getMessage());
        }
        out.flush();
      }
    } catch (IOException | EngineException e) {
      // client went away or engine unreachable; nothing to do
    }
  }

  static void render(QueryResult r, PrintWriter out) {
    switch (r.kind()) {
      case OK -> out.println("OK");
      case NOT_FOUND -> out.println("NOT_FOUND");
      case VALUE -> out.println("VALUE " + Bytes.show(r.value()));
      case COUNT -> out.println("COUNT " + r.count());
      case ROWS -> {
        for (KeyValue kv : r.rows()) out.println("ROW " + kv.keyString() + " " + Bytes.show(kv.value()));
        out.println("END " + r.rows().size());
      }
      case TEXT -> {
        String text = r.text().endsWith("\n") ? r.text().substring(0, r.text().length() - 1) : r.text();
        String[] lines = text.split("\n", -1);
        out.println("TEXT " + lines.length);
        for (String l : lines) out.println(l);
      }
    }
  }
}
