// Server for the WebAssembly build of Ripes with the remote file system
// (RIPES_WITH_REMOTE_FS). It serves the files of Ripes and the file API used by
// src/remotefs/remotefileengine.cpp, storing the files in a single folder.

const express = require("express");
const fs = require("fs");
const path = require("path");

const PORT = process.env.RIPES_LISTEN_PORT || 8080;

// Optional positional arguments: node server.js [DATA_PATH [BASE_PATH]].
// They take precedence over the environment variables.
const [argDataPath, argBasePath] = process.argv.slice(2);

// Folder where the files are stored.
const DATA_PATH =
    argDataPath || process.env.RIPES_DATA_PATH || path.join(__dirname, "data");
fs.mkdirSync(DATA_PATH, { recursive: true });

// Path prefix under which Ripes is served, e.g. "/ripes" (empty at the root).
// Leading and trailing slashes are ignored.
const TRIMMED_BASE_PATH = (argBasePath || process.env.RIPES_BASE_PATH || "").replace(
    /^\/+|\/+$/g,
    "",
);
const BASE_PATH = TRIMMED_BASE_PATH ? `/${TRIMMED_BASE_PATH}` : "";

// Folder with the files produced by the WebAssembly build of Ripes.
const STATIC_PATH = path.join(__dirname, "static");

const app = express();
const router = express.Router();

// Validate the file name and return the path of the file in DATA_PATH. Only
// C and assembly sources directly inside the folder are accepted.
function getSourcePath(filename) {
    if (
        typeof filename !== "string" ||
        !/^[a-zA-Z0-9_-]+\.(s|c)$/.test(filename)
    ) {
        return null;
    }

    return path.join(DATA_PATH, filename);
}

// The multithreaded WebAssembly build uses SharedArrayBuffer, which browsers
// enable only for cross-origin isolated pages.
app.use((req, res, next) => {
    res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
    res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
    next();
});

router.use(express.text());

// List the C and assembly source files (a JSON array of names).
router.get("/api/files", (req, res) => {
    fs.readdir(DATA_PATH, { withFileTypes: true }, (error, entries) => {
        if (error) {
            res.status(500).send("Error listing source files");
            return;
        }

        const files = entries
            .filter((entry) => entry.isFile() && /\.(s|c)$/i.test(entry.name))
            .map((entry) => entry.name);

        res.json(files);
    });
});

// Read a file. Express also answers HEAD requests with this route: Ripes uses
// them to check whether a file exists and to get its size (Content-Length).
router.get("/api/files/:filename", (req, res) => {
    const filePath = getSourcePath(req.params.filename);

    if (!filePath) {
        res.status(400).send("Invalid source filename");
        return;
    }

    fs.readFile(filePath, "utf8", (error, data) => {
        if (error) {
            if (error.code === "ENOENT") {
                res.status(404).send("Source file not found");
                return;
            }

            res.status(500).send("Error reading source file");
            return;
        }

        res.type("text/plain").send(data);
    });
});

// Create or overwrite a file with the body of the request.
router.put("/api/files/:filename", (req, res) => {
    const filePath = getSourcePath(req.params.filename);

    if (!filePath) {
        res.status(400).send("Invalid source filename");
        return;
    }

    fs.writeFile(filePath, req.body, "utf8", (error) => {
        if (error) {
            res.status(500).send("Error writing source file");
            return;
        }

        res.status(200).send("File changes saved correctly");
    });
});

// Files of Ripes, with Ripes.html as the page of the base path.
router.use(express.static(STATIC_PATH, { index: "Ripes.html" }));

// The page must be loaded from a URL ending with a slash, because Ripes
// derives the address of the API from the folder of the page.
if (BASE_PATH) {
    app.get(BASE_PATH, (req, res, next) => {
        if (req.path.endsWith("/")) {
            next();
            return;
        }

        res.redirect(308, `${BASE_PATH}/`);
    });
}

app.use(BASE_PATH || "/", router);

app.listen(PORT, () => {
    console.log(`Ripes served on http://localhost:${PORT}${BASE_PATH}/`);
    console.log(`Files stored in ${DATA_PATH}`);
});
