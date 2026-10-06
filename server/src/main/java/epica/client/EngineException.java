// EngineException.java -- the engine (or the connection to it) reported a
// failure. Distinct from QueryException, which is the user's fault.
package epica.client;

public class EngineException extends RuntimeException {
  public EngineException(String message) { super(message); }
  public EngineException(String message, Throwable cause) { super(message, cause); }
}
