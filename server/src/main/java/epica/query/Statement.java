// Statement.java -- the abstract syntax tree (AST) of the query language.
//
// The parser produces one of these; the planner consumes it. Using a sealed
// interface + records gives an exhaustive, immutable AST with no
// boilerplate. Grammar (case-insensitive keywords):
//
//   statement := PUT key value
//              | GET key
//              | DEL key | DELETE key
//              | SCAN [PREFIX p | [FROM a] [TO b]] [WHERE VALUE CONTAINS s] [LIMIT n]
//              | COUNT [PREFIX p | [FROM a] [TO b]] [WHERE VALUE CONTAINS s]
//              | BATCH (PUT k v | DEL k) (';' (PUT k v | DEL k))* END
//              | EXPLAIN statement
//              | STATS
//   key, value, p, a, b, s := WORD | STRING | NUMBER
package epica.query;

import java.util.List;
import java.util.Optional;

public sealed interface Statement {
  record Put(String key, String value) implements Statement {}

  record Get(String key) implements Statement {}

  record Delete(String key) implements Statement {}

  /** Range of keys to visit: either a prefix, or [from, to) bounds (either may be absent). */
  record Range(Optional<String> prefix, Optional<String> from, Optional<String> to) {
    public static Range all() { return new Range(Optional.empty(), Optional.empty(), Optional.empty()); }
    public static Range prefix(String p) { return new Range(Optional.of(p), Optional.empty(), Optional.empty()); }
    public static Range bounds(String from, String to) {
      return new Range(Optional.empty(), Optional.ofNullable(from), Optional.ofNullable(to));
    }
  }

  /** Optional row predicate. Today only "VALUE CONTAINS s". */
  record Predicate(String valueContains) {}

  record Scan(Range range, Optional<Predicate> where, Optional<Integer> limit) implements Statement {}

  record Count(Range range, Optional<Predicate> where) implements Statement {}

  record BatchOp(boolean isPut, String key, String value) {}

  record Batch(List<BatchOp> ops) implements Statement {}

  record Explain(Statement inner) implements Statement {}

  record Stats() implements Statement {}
}
