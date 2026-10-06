// QueryResult.java -- what executing a plan produces. One of:
//   OK              write succeeded
//   VALUE           a single value (GET hit)
//   NOT_FOUND       GET miss
//   ROWS            zero or more key/value rows (SCAN)
//   COUNT           a number
//   TEXT            free text (STATS, EXPLAIN)
//
// The shell and the query server render these differently; the executor
// does not care about presentation.
package epica.query;

import epica.client.KeyValue;
import java.util.List;

public record QueryResult(Kind kind, byte[] value, List<KeyValue> rows, long count, String text) {
  public enum Kind { OK, VALUE, NOT_FOUND, ROWS, COUNT, TEXT }

  public static QueryResult ok() { return new QueryResult(Kind.OK, null, List.of(), 0, ""); }
  public static QueryResult value(byte[] v) { return new QueryResult(Kind.VALUE, v, List.of(), 0, ""); }
  public static QueryResult notFound() { return new QueryResult(Kind.NOT_FOUND, null, List.of(), 0, ""); }
  public static QueryResult rows(List<KeyValue> r) { return new QueryResult(Kind.ROWS, null, r, r.size(), ""); }
  public static QueryResult count(long n) { return new QueryResult(Kind.COUNT, null, List.of(), n, ""); }
  public static QueryResult text(String t) { return new QueryResult(Kind.TEXT, null, List.of(), 0, t); }
}
