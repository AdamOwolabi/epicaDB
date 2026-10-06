// Parser.java -- recursive-descent parser: tokens -> Statement.
//
// Each grammar rule is one method. Error messages include the token
// position so the shell can point at the problem.
//
//   Parser.parse("SCAN PREFIX user: WHERE VALUE CONTAINS \"ada\" LIMIT 10")
//   -> Scan(Range(prefix=user:), Predicate(ada), limit=10)
package epica.query;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public final class Parser {
  private final List<Token> tokens;
  private int i = 0;

  private Parser(String src) { this.tokens = Lexer.tokenize(src); }

  public static Statement parse(String src) {
    Parser p = new Parser(src);
    Statement s = p.statement();
    p.expectEof();
    return s;
  }

  // --- rules ---

  private Statement statement() {
    Token t = peek();
    if (t.kind() != Token.Kind.WORD) throw error("expected a command", t);
    String cmd = t.text().toUpperCase();
    switch (cmd) {
      case "PUT": {
        advance();
        String k = literal("key");
        String v = literal("value");
        return new Statement.Put(k, v);
      }
      case "GET":
        advance();
        return new Statement.Get(literal("key"));
      case "DEL":
      case "DELETE":
        advance();
        return new Statement.Delete(literal("key"));
      case "SCAN": {
        advance();
        Statement.Range range = range();
        Optional<Statement.Predicate> where = where();
        Optional<Integer> limit = limit();
        return new Statement.Scan(range, where, limit);
      }
      case "COUNT": {
        advance();
        Statement.Range range = range();
        Optional<Statement.Predicate> where = where();
        return new Statement.Count(range, where);
      }
      case "BATCH":
        advance();
        return batch();
      case "EXPLAIN":
        advance();
        return new Statement.Explain(statement());
      case "STATS":
        advance();
        return new Statement.Stats();
      default:
        throw error("unknown command '" + t.text() + "'", t);
    }
  }

  private Statement.Range range() {
    if (peek().isWord("PREFIX")) {
      advance();
      return Statement.Range.prefix(literal("prefix"));
    }
    String from = null, to = null;
    if (peek().isWord("FROM")) {
      advance();
      from = literal("start key");
    }
    if (peek().isWord("TO")) {
      advance();
      to = literal("end key");
    }
    return Statement.Range.bounds(from, to);
  }

  private Optional<Statement.Predicate> where() {
    if (!peek().isWord("WHERE")) return Optional.empty();
    advance();
    expectWord("VALUE");
    expectWord("CONTAINS");
    return Optional.of(new Statement.Predicate(literal("search string")));
  }

  private Optional<Integer> limit() {
    if (!peek().isWord("LIMIT")) return Optional.empty();
    advance();
    Token t = advance();
    if (t.kind() != Token.Kind.NUMBER) throw error("LIMIT needs a number", t);
    int n = Integer.parseInt(t.text());
    if (n <= 0) throw error("LIMIT must be positive", t);
    return Optional.of(n);
  }

  private Statement batch() {
    List<Statement.BatchOp> ops = new ArrayList<>();
    while (true) {
      Token t = peek();
      if (t.isWord("END")) {
        advance();
        break;
      }
      if (t.isWord("PUT")) {
        advance();
        ops.add(new Statement.BatchOp(true, literal("key"), literal("value")));
      } else if (t.isWord("DEL") || t.isWord("DELETE")) {
        advance();
        ops.add(new Statement.BatchOp(false, literal("key"), ""));
      } else {
        throw error("expected PUT, DEL or END inside BATCH", t);
      }
      if (peek().kind() == Token.Kind.SEMI) advance();
    }
    if (ops.isEmpty()) throw error("empty BATCH", peek());
    return new Statement.Batch(ops);
  }

  // --- helpers ---

  /** A key/value literal: WORD, STRING or NUMBER. */
  private String literal(String what) {
    Token t = advance();
    switch (t.kind()) {
      case WORD:
      case STRING:
      case NUMBER:
        return t.text();
      default:
        throw error("expected " + what, t);
    }
  }

  private void expectWord(String w) {
    Token t = advance();
    if (!t.isWord(w)) throw error("expected " + w, t);
  }

  private void expectEof() {
    Token t = peek();
    if (t.kind() != Token.Kind.EOF) throw error("unexpected trailing input '" + t.text() + "'", t);
  }

  private Token peek() { return tokens.get(i); }

  private Token advance() {
    Token t = tokens.get(i);
    if (t.kind() != Token.Kind.EOF) i++;
    return t;
  }

  private static QueryException error(String msg, Token at) {
    return new QueryException(msg + " (at position " + at.position() + ")");
  }
}
