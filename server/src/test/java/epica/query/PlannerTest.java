// PlannerTest.java -- the physical plan shapes and the prefix bound math.
package epica.query;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

class PlannerTest {
  private final Planner planner = new Planner();

  @Test
  void prefixSuccessor() {
    assertArrayEquals(Bytes.utf8("user;"), Bytes.prefixSuccessor(Bytes.utf8("user:")));
    assertArrayEquals(new byte[] {'a', 'c'}, Bytes.prefixSuccessor(new byte[] {'a', 'b', (byte) 0xff}));
    assertArrayEquals(new byte[0], Bytes.prefixSuccessor(new byte[] {(byte) 0xff, (byte) 0xff}));
    assertArrayEquals(new byte[0], Bytes.prefixSuccessor(new byte[0]));
  }

  @Test
  void prefixScanBecomesBoundedRangeScan() {
    PlanNode p = planner.plan(Parser.parse("SCAN PREFIX user:"));
    assertTrue(p instanceof PlanNode.RangeScan);
    PlanNode.RangeScan s = (PlanNode.RangeScan) p;
    assertArrayEquals(Bytes.utf8("user:"), s.start());
    assertArrayEquals(Bytes.utf8("user;"), s.end());
    assertEquals(0, s.pushdownLimit());
  }

  @Test
  void limitIsPushedDownOnlyWithoutFilter() {
    PlanNode.Limit l1 = (PlanNode.Limit) planner.plan(Parser.parse("SCAN LIMIT 5"));
    assertEquals(5, ((PlanNode.RangeScan) l1.child()).pushdownLimit());

    PlanNode.Limit l2 = (PlanNode.Limit) planner.plan(Parser.parse("SCAN WHERE VALUE CONTAINS x LIMIT 5"));
    PlanNode.Filter f = (PlanNode.Filter) l2.child();
    assertEquals(0, ((PlanNode.RangeScan) f.child()).pushdownLimit(), "engine cannot apply a filtered limit");
  }

  @Test
  void countWrapsScan() {
    PlanNode.Count c = (PlanNode.Count) planner.plan(Parser.parse("COUNT FROM a TO b"));
    PlanNode.RangeScan s = (PlanNode.RangeScan) c.child();
    assertArrayEquals(Bytes.utf8("a"), s.start());
    assertArrayEquals(Bytes.utf8("b"), s.end());
  }

  @Test
  void explainRendersTree() {
    PlanNode p = planner.plan(Parser.parse("EXPLAIN SCAN PREFIX u WHERE VALUE CONTAINS x LIMIT 2"));
    String text = ((PlanNode.Explain) p).inner().explain(0);
    assertTrue(text.startsWith("Limit(2)\n  Filter(value CONTAINS \"x\")\n    RangeScan(start=\"u\", end=\"v\""), text);
  }
}
