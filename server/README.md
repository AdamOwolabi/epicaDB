# epicaDB query layer (Java 17)

Parses the query language, plans it, and executes it against the C++ storage
engine over the binary protocol in `engine/net/protocol.h`.

```
src/main/java/epica/
  Main.java                    entry: shell | memory | server
  client/
    EngineClient.java          the interface the executor is written against
    SocketEngineClient.java    real engine over TCP
    InMemoryEngineClient.java  TreeMap fake for tests / demos
    Protocol.java              little-endian frames, byte-exact with the C++ side
  query/
    Lexer.java  Token.java     text -> tokens
    Parser.java Statement.java tokens -> AST (sealed interface + records)
    Planner.java PlanNode.java AST -> physical plan (prefix bounds, limit pushdown)
    Executor.java              pull-based operators: RangeScan (paged) / Filter / Limit / Count
    QueryResult.java Bytes.java
  shell/Shell.java             REPL
  server/QueryServer.java      line protocol over TCP (nc-friendly)
src/test/java/epica/           JUnit 5: 21 unit tests + 1 gated integration test
```

```bash
export JAVA_HOME=$(/usr/libexec/java_home -v 17)
mvn -q package                                   # tests + target/epica-query.jar
java -jar target/epica-query.jar shell 127.0.0.1:7379
java -jar target/epica-query.jar server 7380 127.0.0.1:7379
java -jar target/epica-query.jar memory          # no engine needed
EPICA_ENGINE_PORT=7379 mvn -q test -Dtest=EngineIntegrationTest
```

See `../BEGINNER_OVERVIEW.md` §"The query layer" for a walkthrough of one
query and `../DESIGN_DECISIONS.md` §17–18 for why it is shaped this way.
