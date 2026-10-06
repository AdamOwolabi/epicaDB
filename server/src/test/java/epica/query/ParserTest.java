// ParserTest.java -- grammar coverage and error reporting.
package epica.query;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Optional;
import org.junit.jupiter.api.Test;

class ParserTest {
  @Test
  void pointCommands() {
    assertEquals(new Statement.Put("k", "v w"), Parser.parse("PUT k \"v w\""));
    assertEquals(new Statement.Get("k"), Parser.parse("get k"));
    assertEquals(new Statement.Delete("k"), Parser.parse("DELETE k"));
    assertEquals(new Statement.Stats(), Parser.parse("STATS"));
  }

  @Test
  void scanVariants() {
    Statement.Scan all = (Statement.Scan) Parser.parse("SCAN");
    assertEquals(Statement.Range.all(), all.range());
    assertTrue(all.where().isEmpty() && all.limit().isEmpty());

    Statement.Scan pfx = (Statement.Scan) Parser.parse("SCAN PREFIX user: LIMIT 10");
    assertEquals(Optional.of("user:"), pfx.range().prefix());
    assertEquals(Optional.of(10), pfx.limit());

    Statement.Scan rng = (Statement.Scan) Parser.parse("SCAN FROM a TO z WHERE VALUE CONTAINS \"x y\"");
    assertEquals(Optional.of("a"), rng.range().from());
    assertEquals(Optional.of("z"), rng.range().to());
    assertEquals("x y", rng.where().get().valueContains());

    Statement.Scan toOnly = (Statement.Scan) Parser.parse("SCAN TO m");
    assertTrue(toOnly.range().from().isEmpty());
    assertEquals(Optional.of("m"), toOnly.range().to());
  }

  @Test
  void countAndExplainAndBatch() {
    Statement.Count c = (Statement.Count) Parser.parse("COUNT PREFIX p WHERE VALUE CONTAINS q");
    assertEquals(Optional.of("p"), c.range().prefix());
    Statement.Explain e = (Statement.Explain) Parser.parse("EXPLAIN GET k");
    assertEquals(new Statement.Get("k"), e.inner());
    Statement.Batch b = (Statement.Batch) Parser.parse("BATCH PUT a 1; DEL b; PUT c \"3\" END");
    assertEquals(3, b.ops().size());
    assertTrue(b.ops().get(0).isPut());
    assertFalse(b.ops().get(1).isPut());
    assertEquals("3", b.ops().get(2).value());
  }

  @Test
  void errorsNamePositions() {
    QueryException e = assertThrows(QueryException.class, () -> Parser.parse("PUT k"));
    assertTrue(e.getMessage().contains("expected value"), e.getMessage());
    assertThrows(QueryException.class, () -> Parser.parse("FROB x"));
    assertThrows(QueryException.class, () -> Parser.parse("SCAN LIMIT 0"));
    assertThrows(QueryException.class, () -> Parser.parse("SCAN LIMIT ten"));
    assertThrows(QueryException.class, () -> Parser.parse("GET a b"));
    assertThrows(QueryException.class, () -> Parser.parse("BATCH END"));
  }
}
