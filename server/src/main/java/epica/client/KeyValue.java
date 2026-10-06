// KeyValue.java -- one row returned by a scan. Keys and values are raw bytes;
// the engine never interprets them, so neither does the client.
package epica.client;

import java.nio.charset.StandardCharsets;

public record KeyValue(byte[] key, byte[] value) {
  public String keyString() { return new String(key, StandardCharsets.UTF_8); }
  public String valueString() { return new String(value, StandardCharsets.UTF_8); }
}
