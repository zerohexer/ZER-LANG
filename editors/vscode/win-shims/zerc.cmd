@echo off
REM zerc terminal shim — runs the WASM compiler via the bundled (signed) node.
REM %~dp0 = ...\bin\win32-x64\  ;  CLI + wasm live in ...\lsp\
"%~dp0node.exe" "%~dp0..\..\lsp\zerc-cli.js" %*
