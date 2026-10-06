// Shell.java -- interactive REPL for the query language.
//
//   epica> PUT user:1 "Ada Lovelace"
//   OK
//   epica> SCAN PREFIX user: LIMIT 5
//   user:1 = "Ada Lovelace"
//   (1 row)
//   epica> EXPLAIN SCAN PREFIX user: WHERE VALUE CONTAINS "Ada" LIMIT 5
//   Limit(5)
//     Filter(value CONTAINS "Ada")
//       RangeScan(start="user:", end="user;", pageSize=128)
//
// Each line is one statement: lexed, parsed, planned and executed by the
// query package. Rendering of results lives here; the query server renders
// the same QueryResult differently (machine-friendly lines).
package epica.shell;

import epica.client.EngineClient;
import epica.client.EngineException;
import epica.client.KeyValue;
import epica.query.Bytes;
import epica.query.Executor;
import epica.query.QueryException;
import epica.query.QueryResult;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;

public final class Shell {
  private final Executor executor;
  private final BufferedReader in;
  private final PrintStream out;

  public Shell(EngineClient engine, InputStream in, PrintStream out) {
    this.executor = new Executor(engine);
    this.in = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
    this.out = out;
  }

  public void run() throws IOException {
    out.println("epicaDB query shell. Commands: PUT GET DEL SCAN COUNT BATCH EXPLAIN STATS; 'help' or 'quit'.");
    String line;
    while (true) {
      out.print("epica> ");
      out.flush();
      line = in.readLine();
      if (line == null) break;
      line = line.trim();
      if (line.isEmpty()) continue;
      if (line.equalsIgnoreCase("quit") || line.equalsIgnoreCase("exit")) break;
      if (line.equalsIgnoreCase("help")) {
        printHelp();
        continue;
      }
      long t0 = System.nanoTime();
      try {
        QueryResult r = executor.execute(line);
        render(r, (System.nanoTime() - t0) / 1_000_000.0);
      } catch (QueryException e) {
        out.println("syntax error: " + e.getMessage());
      } catch (EngineException e) {
        out.println("engine error: " + e.getMessage());
      }
    }
  }

  private void render(QueryResult r, double ms) {
    switch (r.kind()) {
      case OK -> out.printf("OK (%.2f ms)%n", ms);
      case NOT_FOUND -> out.printf("(not found) (%.2f ms)%n", ms);
      case VALUE -> out.printf("%s (%.2f ms)%n", Bytes.show(r.value()), ms);
      case COUNT -> out.printf("%d (%.2f ms)%n", r.count(), ms);
      case TEXT -> out.print(r.text().endsWith("\n") ? r.text() : r.text() + "\n");
      case ROWS -> {
        for (KeyValue kv : r.rows()) out.println(kv.keyString() + " = " + Bytes.show(kv.value()));
        out.printf("(%d row%s, %.2f ms)%n", r.rows().size(), r.rows().size() == 1 ? "" : "s", ms);
      }
    }
  }

  private void printHelp() {
    out.println("""
        PUT <key> <value>                      write (value may be "quoted with spaces")
        GET <key>                              read
        DEL <key>                              delete
        SCAN [PREFIX p | FROM a [TO b]] [WHERE VALUE CONTAINS s] [LIMIT n]
        COUNT [PREFIX p | FROM a [TO b]] [WHERE VALUE CONTAINS s]
        BATCH PUT k v; DEL k2; ... END         atomic multi-op write
        EXPLAIN <statement>                    show the physical plan
        STATS                                  engine statistics""");
  }
}
