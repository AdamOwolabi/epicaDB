// EngineIntegrationTest.java -- runs the Java query layer against a REAL
// C++ engine server. Skipped unless EPICA_ENGINE_PORT is set, so plain
// `mvn test` never needs the server. TESTING.md shows how to run it.
package epica.integration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import epica.client.SocketEngineClient;
import epica.query.Bytes;
import epica.query.Executor;
import epica.query.QueryResult;
import org.junit.jupiter.api.Test;

class EngineIntegrationTest {
  @Test
  void roundTripThroughRealEngine() {
    String port = System.getenv("EPICA_ENGINE_PORT");
    assumeTrue(port != null && !port.isEmpty(), "EPICA_ENGINE_PORT not set; skipping");
    try (SocketEngineClient client = SocketEngineClient.connect("127.0.0.1", Integer.parseInt(port))) {
      client.ping();
      Executor ex = new Executor(client);
      String ns = "itest:" + System.nanoTime() + ":";
      for (int i = 0; i < 300; i++) ex.execute("PUT " + ns + String.format("%03d", i) + " val" + (i % 4));
      assertEquals("val1", Bytes.str(ex.execute("GET " + ns + "001").value()));
      assertEquals(300, ex.execute("COUNT PREFIX " + ns).count());
      assertEquals(75, ex.execute("COUNT PREFIX " + ns + " WHERE VALUE CONTAINS val2").count());
      QueryResult page = ex.execute("SCAN PREFIX " + ns + " LIMIT 7");
      assertEquals(7, page.rows().size());
      ex.execute("BATCH DEL " + ns + "000; PUT " + ns + "new x END");
      assertEquals(QueryResult.Kind.NOT_FOUND, ex.execute("GET " + ns + "000").kind());
      assertTrue(ex.execute("STATS").text().contains("last_sequence"));
      // clean up
      for (int i = 1; i < 300; i++) ex.execute("DEL " + ns + String.format("%03d", i));
      ex.execute("DEL " + ns + "new");
      assertEquals(0, ex.execute("COUNT PREFIX " + ns).count());
    }
  }
}
