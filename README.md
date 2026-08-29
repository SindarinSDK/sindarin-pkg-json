# Sindarin JSON

A zero-dependency JSON encoder/decoder for the [Sindarin](https://github.com/SindarinSDK/sindarin-compiler) programming language. Provides fast JSON serialization and deserialization for `@serializable` structs using direct string building — no external libraries required.

## Installation

Add the package as a dependency in your `sn.yaml`:

```yaml
dependencies:
- name: sindarin-pkg-json
  git: git@github.com:SindarinSDK/sindarin-pkg-json.git
  branch: main
```

Then run `sn --install` to fetch the package.

## Quick Start

Define a `@serializable` struct and encode/decode it:

```sindarin
import "json/json"

@serializable
struct Person =>
    name: str
    age: int
    active: bool

fn main(): void =>
    // Encode
    var p: Person = Person { name: "Alice", age: 30, active: true }
    var jsonStr: str = p.encode(Json.encoder())
    println(jsonStr)   // {"name":"Alice","age":30,"active":true}

    // Decode
    var p2: Person = Person.decode(Json.decoder(jsonStr))
    println(p2.name)   // Alice
```

## Documentation

### Module

| Module | Import | Description |
|--------|--------|-------------|
| Json | `import "json/json"` | JSON encoder/decoder for `@serializable` structs |

### Json

Static factory methods for creating encoders and decoders.

```sindarin
import "json/json"
```

#### Encoding

```sindarin
Json.encoder(): Encoder          # Create an object encoder (produces {...})
Json.arrayEncoder(): Encoder     # Create an array encoder (produces [...])
```

Pass the returned `Encoder` to a struct's generated `.encode()` method. The method finalizes the encoder and returns the JSON string.

```sindarin
@serializable
struct Address =>
    street: str
    city: str

var a: Address = Address { street: "123 Main St", city: "NYC" }
var json: str = a.encode(Json.encoder())
// {"street":"123 Main St","city":"NYC"}
```

#### Decoding

```sindarin
Json.decoder(input: str): Decoder        # Parse a JSON string (object or array)
Json.arrayDecoder(input: str): Decoder   # Alias for decoder
```

The returned `Decoder` is passed to a struct's generated `.decode()` static method.

```sindarin
var dec: Decoder = Json.decoder("{\"street\":\"123 Main St\",\"city\":\"NYC\"}")
var a: Address = Address.decode(dec)
println(a.street)  // 123 Main St
```

### Supported Types

The encoder/decoder handles all types supported by `@serializable`:

| Type | JSON Representation |
|------|-------------------|
| `str` | `"string"` |
| `int` | `123` |
| `double` | `9.5` |
| `bool` | `true` / `false` |
| Nested structs | `{...}` |
| Arrays (`T[]`) | `[...]` |

String values are automatically escaped during encoding (quotes, newlines, tabs, etc.) and unescaped during decoding.

### Nested Structs and Arrays

`@serializable` structs can contain other `@serializable` structs and arrays:

```sindarin
@serializable
struct Address =>
    street: str
    city: str

@serializable
struct Person =>
    name: str
    age: int
    address: Address
    tags: str[]

@serializable
struct Team =>
    name: str
    members: Person[]

var team: Team = Team {
    name: "Engineering",
    members: {
        Person { name: "Alice", age: 30, address: Address { street: "1 A", city: "X" }, tags: {"dev"} },
        Person { name: "Bob", age: 25, address: Address { street: "2 B", city: "Y" }, tags: {"ops", "sre"} }
    }
}

var json: str = team.encode(Json.encoder())

// Roundtrip back to a struct
var team2: Team = Team.decode(Json.decoder(json))
```

## Development

### Running Tests

```bash
make test
```

### Available Targets

```bash
make test       # Run tests
make clean      # Remove build artifacts
make help       # Show all targets
```

## Dependencies

This package depends on [sindarin-pkg-test](https://github.com/SindarinSDK/sindarin-pkg-test) for the test runner. Dependencies are automatically managed via the `sn.yaml` package manifest.

## License

MIT License
