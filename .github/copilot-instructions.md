# C++ Coding Standards

## Member Variable Naming

- All class member variables must use the format:
  m_variable_name
- Use snake_case after the prefix.
- Example:
    int m_buffer_size;
    std::string m_user_name;

- Do NOT use trailing underscores.
- Do NOT omit the m_ prefix.

## Method Naming

- Use PascalCase for member functions.
- Accessors:
    GetValue()
    SetValue()
- For method/function paramter names, use snake_case:
    void SetBufferSize(int buffer_size);

## Braces placement style

- Use Allman-style brace formatting: place every opening curly brace `{` on its own line, aligned with the corresponding declaration or control statement (e.g., methods, conditionals, loops, classes).
- Never place an opening brace `{` on the same line as the preceding statement.

## Const Correctness

- Mark methods const when they do not modify state.
- Pass large objects by const reference.
- Use constexpr where appropriate.

## Comment Format

- Use this comment format for all public classes and methods.
- Format:

/**
Description of the class/method/function here. Description details should vary 
based on the complexity of the code.

If needed/not self-explanatory, describe parameters and return value:
Parameters:
  param_name: Description.

Returns: Description.
**/

- Use // comments only for short inline explanations.
