@echo off
rem Starts the AI overlay's bridge (see bridge.py) in the StreamDiffusion environment. Leave it running while playing.
"%~dp0..\..\StreamDiffusion\.venv\Scripts\python.exe" "%~dp0bridge.py" %*
