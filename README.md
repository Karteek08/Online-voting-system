# NIT Silchar Gymkhana Online Voting System

A demo online voting application with a plain HTML, CSS, and JavaScript frontend and a C backend. The backend serves the website and provides the voting API.

## Requirements

- Windows: MinGW GCC (including the Winsock library)
- Linux or macOS: GCC or another compatible C compiler

## Build

Open a terminal in the project folder and compile the server:

**Windows (MinGW):**

```powershell
gcc server.c -o server.exe -lws2_32
```

**Linux or macOS:**

```sh
gcc server.c -o server
```

## Run

Run the server from the project folder so it can find `index.html`, `app.js`, and `style.css`.

**Windows:**

```powershell
.\server.exe
```

**Linux or macOS:**

```sh
./server
```

Then open <http://localhost:8080> in a browser. Keep the terminal open while using the site. Press **Ctrl+C** in that terminal to stop the server.

The server listens on port `8080` and only accepts connections from the local computer.

## Demo notes

- The admin demo password is `admin123`.
- No email service is configured. The generated OTP is shown on the login page.
- Election data is held in memory and resets when the server restarts.
- This project is for demonstration and coursework only; it is not suitable for conducting a real election.

## Project files

- `server.c` — HTTP server and voting API
- `index.html` — application page
- `app.js` — frontend behavior
- `style.css` — page styling
