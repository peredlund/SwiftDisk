# SwiftDisk Instructions

These instructions apply to the entire repository unless a more specific `AGENTS.md` exists in a subdirectory.

## C++ Coding Standards

### Member Variable Naming

- All class member variables must use the format `m_variable_name`.
- Use snake_case after the prefix.
- Example:
  `int m_buffer_size;`
  `std::string m_user_name;`
- Do not use trailing underscores.
- Do not omit the `m_` prefix.

### Method Naming

- Use PascalCase for member functions.
- Accessors should follow patterns like `GetValue()` and `SetValue()`.
- Use snake_case for method and function parameter names.
- Example:
  `void SetBufferSize(int buffer_size);`

### Brace Placement Style

- Use Allman-style brace formatting.
- Place every opening curly brace `{` on its own line, aligned with the corresponding declaration or control statement, including methods, conditionals, loops, classes, and namespaces.
- Never place an opening brace `{` on the same line as the preceding statement.

### Const Correctness

- Mark methods `const` when they do not modify state.
- Pass large objects by `const` reference.
- Use `constexpr` where appropriate.

### Comment Format

- Use the following documentation comment format for public classes and methods:

```cpp
/**
Description of the class/method/function here. Description details should vary
based on the complexity of the code.

If needed or not self-explanatory, describe parameters and return value:
Parameters:
  param_name: Description.

Returns: Description.
**/
```

- Use `//` comments only for short inline explanations.

## OpenAI Codex Windows App Instructions

Before running `cmake --build` or other MSBuild-based commands in PowerShell, work around duplicate `PATH`/`Path` process variables with:

```powershell
[System.Environment]::SetEnvironmentVariable('PATH', $env:Path, 'Process')
[System.Environment]::SetEnvironmentVariable('Path', $null, 'Process')
```

Then run the build command.
