# Contributing to Vectora Server

First off, thank you for considering contributing to VectoraServer! It’s people like you who make this a high-performance, community-driven Minecraft server implementation.

By participating in this project, you agree to abide by our Code of Conduct.

# How Can I Contribute?
## Reporting Bugs

If you find a bug, please open an Issue and include:

    The server version (and the Minecraft client version used).

    Steps to reproduce the bug.

    Expected behavior vs. Actual behavior.

    Crash logs or terminal output if applicable.

## Suggesting Enhancements

We are always looking to optimize the server. If you have an idea for a performance improvement or a missing protocol feature:

    Check if the idea has already been suggested in the Issues.

    Explain why this enhancement would be useful.

    If it involves the Minecraft Protocol, please link to relevant documentation (e.g., wiki.vg).

## Pull Requests

    Fork the repo and create your branch from main.

    Ensure code style consistency. Since this is a C project, we follow specific formatting rules (see Coding Standards below).

    Test your changes. Verify that your changes don't break existing protocol handshakes or world loading.

    Submit the PR with a clear description of what you changed and why.

## Coding Standards

To maintain high performance and memory safety in C, please follow these guidelines:
1. Memory Management

    No Memory Leaks: All allocated memory must be freed correctly. Use tools like valgrind to check your work.

    Buffer Safety: Avoid unsafe functions like strcpy or sprintf. Use strncpy, snprintf, or internal safe buffer utilities.

2. Style & Formatting

    Indentation: Use 4 spaces (no tabs).

    Naming: Use snake_case for functions and variables. Use SCREAMING_SNAKE_CASE for constants and macros.

    Comments: Use Doxygen-style comments for public header functions.

3. Protocol Implementation

    When implementing new packets, ensure they align strictly with the Minecraft Protocol (1.21+).

    Keep the NBT and Anvil handling logic decoupled from the networking logic.