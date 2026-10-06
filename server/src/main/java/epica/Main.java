// Main.java -- entry point for the Java layer.
//
//   java -jar target/epica-query.jar shell  [host:port]      interactive REPL
//   java -jar target/epica-query.jar server [listenPort] [engineHost:port]
//   java -jar target/epica-query.jar memory                  REPL on an in-memory fake engine
//
// Defaults: engine at 127.0.0.1:7379, query server listens on 7380.
package epica;

import epica.client.EngineClient;
import epica.client.InMemoryEngineClient;
import epica.client.SocketEngineClient;
import epica.server.QueryServer;
import epica.shell.Shell;

public final class Main {
  public static void main(String[] args) throws Exception {
    String mode = args.length > 0 ? args[0] : "shell";
    switch (mode) {
      case "shell" -> {
        String target = args.length > 1 ? args[1] : "127.0.0.1:7379";
        try (SocketEngineClient client = SocketEngineClient.connect(target)) {
          new Shell(client, System.in, System.out).run();
        }
      }
      case "memory" -> {
        EngineClient client = new InMemoryEngineClient();
        new Shell(client, System.in, System.out).run();
      }
      case "server" -> {
        int port = args.length > 1 ? Integer.parseInt(args[1]) : 7380;
        String target = args.length > 2 ? args[2] : "127.0.0.1:7379";
        QueryServer server = new QueryServer(port, () -> SocketEngineClient.connect(target));
        server.start();
        System.out.println("epica query server listening on " + port + ", engine at " + target);
        server.join();
      }
      default -> {
        System.err.println("usage: shell [host:port] | memory | server [port] [engineHost:port]");
        System.exit(64);
      }
    }
  }
}
