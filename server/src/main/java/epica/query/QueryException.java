// QueryException.java -- the query text was invalid (syntax error, unknown
// command, bad LIMIT). The user's fault, reported back as an ERROR line.
package epica.query;

public class QueryException extends RuntimeException {
  public QueryException(String message) { super(message); }
}
