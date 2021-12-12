set shell := ["bash", "-c"]

hw := "8"
team_name := "FireFerrises"
n_proc := `nproc`
just_dir := justfile_directory()
cwd := invocation_directory()
sudo := "sudo -E env \"PATH=${PATH}\""

default:
    just --list --unsorted

install-program-dependencies:
    {{sudo}} apt install \
        build-essential \
        curl \
        make \
        python \
        clang-format \
        bear \
        "linux-headers-$(uname -r)"
    command -v cargo > /dev/null || curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
    cargo install cargo-quickinstall
    cargo quickinstall just
    cargo quickinstall ripgrep
    cargo quickinstall fd-find
    cargo quickinstall sd
    cargo quickinstall sk
    cargo quickinstall gitui
    cargo quickinstall watchexec

parallel-bash-commands:
    echo 'pids=()'
    -rg '^(.*)$' --replace '$1 &'$'\n''pids+=($!)' --color never
    echo 'for pid in ${pids[@]}; do wait $pid; done'

sequential-bash:
    bash

parallel-bash:
    just parallel-bash-commands | bash

map-lines pattern replacement:
    -rg '^{{pattern}}$' --replace '{{replacement}}' --color never

pre-make:

make-with-makefile path *args:
    cd "{{parent_directory(path)}}" && make --makefile "{{file_name(path)}}" -j{{n_proc}} {{args}}

make-in dir *args: (make-with-makefile join(dir, "Makefile") args)

make-in-recursive dir *args:
    fd '^Makefile$' "{{dir}}" \
        | just map-lines '(.*)' 'just make-with-makefile "$1" {{args}}' \
        | just parallel-bash

git-clone repo *args=("--recursive"):
    cd "{{invocation_directory()}}" && \
        command -v gh >/dev/null \
        && gh repo clone "{{repo}}" -- {{args}} \
        || git clone "https://github.com/{{repo}}" {{args}}

reinstall-kedr:
    #!/usr/bin/env bash
    set -euxo pipefail

    rm -rf ~/kedr
    just git-clone hmontero1205/kedr ~/kedr
    cd ~/kedr/sources
    mkdir build
    cd build
    cmake ..
    make -j{{n_proc}}
    {{sudo}} make install

# Paranthesized deps to avoid checkpatch repeated word warning
make *args: (pre-make) (make-in "." args)

first-commit:
    git rev-list --max-parents=0 HEAD

modified-files *args:
    cd "{{cwd}}" && git diff --name-only {{args}}

# clang-format formats some stuff wrong, so re-format it
# specifically for each macros
refmt *paths:
    #!/usr/bin/env node
    const fsp = require("fs/promises");
    const pathLib = require("path");

    async function reformatCFile(path) {
        let s = await fsp.readFile(path);
        s = s.toString();
        const original = s;
        // fix for each macros
        s = s.replaceAll(/([a-z_]*for_each[a-z_]*) /g, (_, macroName) => macroName);
        const to = s;
        const changed = s !== original;
        if (changed) {
            // only update timestamps if we really need to change something
            await fsp.writeFile(path, s);
        }
        return changed;
    }

    async function main() {
        // const [_node, _program, ...paths] = process.argv;
        const pathsGiven = `{{paths}}`.split(" ");
        let it = pathsGiven;
        it = it.filter(path => ["c", "h"].includes(pathLib.extname(path).slice(1)));
        it = [...new Set(it)];
        const pathsFormatting = it;
        console.log(["reformatting:", ...pathsFormatting].join("\n\t"));
        it = it.map(async (path, i) => ({path, formatted: await reformatCFile(path)}));
        it = await Promise.all(it);
        const pathsFormatted = it.filter(e => e.formatted).map(e => e.path);
        console.log(["reformatted:", ...pathsFormatted].join("\n\t"));
    }

    main().catch(e => {
        console.error(e);
        process.exit(1);
    });

fmt *args:
    git clang-format --force {{args}}
    just refmt $(just modified-files {{args}})

entire-diff *files:
    cd "{{cwd}}" && git diff "$(just first-commit)" -- {{files}}

pre-commit-fast: fmt check-patch

pre-commit-slow: make

pre-commit: pre-commit-fast pre-commit-slow

gitui: pre-commit
    gitui

compile-commands: (make "clean")
    bear -- just make all
    command -v ccache > /dev/null \
        && sd "$(which ccache)" "$(which gcc)" compile_commands.json \
        || true

log *args:
    {{sudo}} dmesg --kernel --reltime {{args}}

log-watch *args: (log "--follow-new" args)

use_kedr := "false"

kedr-stop-no-sudo:
    #!/usr/bin/env bash
    set -euo pipefail

    [[ "{{use_kedr}}" != "true" ]] && exit
    cd /sys/kernel/debug/kedr_leak_check
    bat --paging never info possible_leaks unallocated_frees
    kedr stop

run-mod mod_path *args:
    #!/usr/bin/env bash
    set -uo pipefail

    just log | wc -l > log.length
    [[ "{{use_kedr}}" == "true" ]] && sudo kedr start "{{mod_path}}"
    echo "running $(tput setaf 2){{file_stem(mod_path)}}$(tput sgr 0):"
    just load-mod "{{mod_path}}"
    {{args}}
    just unload-mod-by-path "{{mod_path}}"
    just log --color=always | tail -n "+$(($(cat log.length) + 1))"
    rm log.length
    {{sudo}} just kedr-stop-no-sudo

test-cmd *args: (run-mod default_mod_path args)

test: (test-cmd "just" "make-in" join("user/test", team_name) "test")

current-branch:
    git branch --show-current || git rev-parse --abbrev-ref HEAD

default-branch:
    git remote show origin | rg 'HEAD branch: (.*)$' --only-matching --replace '$1'

tag name message:
    git tag -a -m "{{message}}" "{{name}}"
    git push origin "$(just current-branch)"
    git push origin "{{name}}"

untag name:
    git push --delete origin "{{name}}"
    git tag --delete "{{name}}"

checkout-tag:
    git checkout "hw{{hw}}handin"

submit-part part_num:
    git checkout "part{{part_num}}"
    just tag "hw{{hw}}p{{part_num}}handin" "Completed hw{{hw}} part{{part_num}}."

unsubmit-part part_num:
    just untag "hw{{hw}}p{{part_num}}handin"

submit:
    for i in {1..10}; do just submit-part $i; done

unsubmit:
    for i in {1..10}; do just unsubmit-part $i; done

diff-command:
    command -v delta > /dev/null && echo delta || echo diff

diff a b:
    "$(just diff-command)" "{{a}}" "{{b}}"

default_mod_path := "mypantry.ko"

is-mod-loaded name=file_stem(default_mod_path):
    rg --quiet '^{{name}} ' /proc/modules

# `path` is `path_` instead to appease checkpatch's repeated word warning
is-mod-loaded-by-path path_=default_mod_path: (is-mod-loaded file_stem(path_))

load-mod path=default_mod_path:
    just unload-mod-by-path "{{path}}"
    {{sudo}} insmod "{{path}}"

unload-mod name=file_stem(default_mod_path):
    just is-mod-loaded "{{name}}" 2> /dev/null && {{sudo}} rmmod "{{name}}" || true

# `path` is `path_` instead to appease checkpatch's repeated word warning
unload-mod-by-path path_=default_mod_path: (unload-mod file_stem(path_))

check_patch_ignores_common := "FILE_PATH_CHANGES,SPDX_LICENSE_TAG,MISSING_EOF_NEWLINE"
check_patch_ignores_hw := "ENOSYS,AVOID_EXTERNS,LINE_CONTINUATIONS,TRAILING_SEMICOLON"

raw-check-patch *args:
    ./util/checkpatch.pl --notree --max-line-length=80 --ignore "{{check_patch_ignores_common}},{{check_patch_ignores_hw}}" {{args}}

check-patch *files:
    cd "{{cwd}}" && just entire-diff {{files}} | just raw-check-patch

filter-exec:
    #!/usr/bin/env node
    const fs = require("fs");
    const pathLib = require("path");

    const which = (() => {
        const cache = new Map();
        const paths = process.env.PATH.split(pathLib.delimiter);
        return program => {
            if (cache.has(program)) {
                return cache.get(program);
            }
            for (const dir of paths) {
                const path = pathLib.join(dir, program);
                let found = false;
                try {
                    fs.accessSync(path, fs.constants.X_OK);
                    found = true;
                } catch {}
                if (found) {
                    cache.set(program, path);
                    return path;
                }
            }
            cache.set(program, undefined);
            return undefined;
        };
    })();

    const colors = {
        reset: 0,
        bright: 1,
        dim: 2,
        underscore: 4,
        blink: 5,
        reverse: 7,
        hidden: 8,
        fg: {
            black: 30,
            red: 31,
            green: 32,
            yellow: 33,
            blue: 34,
            magenta: 35,
            cyan: 36,
            white: 37,
        },
        bg: {
            black: 40,
            red: 41,
            green: 42,
            yellow: 43,
            blue: 44,
            magenta: 45,
            cyan: 46,
            white: 47,
        },
    };

    function ansiColorSequence(colorCode) {
        return `\x1b[${colorCode}m`;
    }

    const color = !process.stdout.isTTY ? ((colorCode, s) => s) : (colorCode, s) => {
        if (colorCode === undefined) {
            throw new Error("undefined color");
        }
        return ansiColorSequence(colorCode) + s + ansiColorSequence(colors.reset);
    };

    function quote(s) {
        return s.includes(" ") ? `"${s}"` : s;
    }

    function colorPath(path, dirColor, nameColor) {
        const {dir, base} = pathLib.parse(path);
        return (dir ? color(dirColor, dir + pathLib.sep) : "") + color(nameColor, base);
    }

    const output = fs.readFileSync("/dev/stdin")
        .toString()
        .split("\n")
        .map(s => {
            const match = /execve\(([^)]*)\) = 0/.exec(s);
            if (!match) {
                return;
            }
            const [_, argsStr] = match;
            const args = argsStr.replaceAll(/\[|\]/g, "").split(", ");
            return args
                .map(rawArgs => {
                    if (!(rawArgs.endsWith('"') && rawArgs.endsWith('"'))) {
                        return;
                    }
                    const arg = rawArgs.slice('"'.length, -'"'.length);
                    return arg;
                })
                .filter(Boolean);
        })
        .filter(Boolean)
        .map(([path, argv0, ...argv]) => {
            const program = pathLib.basename(path);
            const isInPath = which(program) === path;
            return {
                path: isInPath ? program : path,
                argv0: (argv0 === path || argv0 === program) ? undefined : argv0,
                argv,
            };
        }).map(({path, argv0, argv}) => ({
            path: quote(path),
            argv0: argv0 === undefined ? undefined : quote(argv0),
            argv: argv.map(quote),
        })).map(({path, argv0, argv}) => {
            if (argv0 === undefined) {
                return [
                    colorPath(path, colors.fg.yellow, colors.fg.green),
                    ...argv,
                ];
            } else {
                return [
                    "[" + colorPath(path, colors.fg.yellow, colors.fg.blue) + "]",
                    colorPath(argv0, colors.fg.yellow, colors.fg.green),
                    ...argv,
                ];
            }
        })
        .map(args => args.join(" "))
        .join("\n") + "\n";
    fs.writeFileSync("/dev/stdout", output);

trace-exec *args:
    #!/usr/bin/env bash
    set -euxo pipefail

    cd "{{invocation_directory()}}"
    strace -etrace=execve -f --string-limit 10000 -qq --output strace.$PPID.out {{args}} || true
    just filter-exec < strace.$PPID.out
    rm strace.$PPID.out

rename-branch old_name new_name:
    git checkout "{{old_name}}"
    git branch --move "{{new_name}}"
    git push origin --set-upstream "{{new_name}}"
    git push origin --delete "{{old_name}}"
    git checkout -

# args = files -- extra args
watch-kernel-files *args:
    #!/usr/bin/env node
    const fsp = require("fs/promises");
    const pathLib = require("path");
    const assert = require("assert/strict");
    const fs = require("fs");
    const childProcess = require("child_process");
    const os = require("os");

    function groupBy(a, f) {
        const o = {};
        a.forEach((e, i, a) => {
            const groupName = f(e, i, a);
            const group = o[groupName] ?? (o[groupName] = []);
            group.push(e);
        });
        return o;
    }

    function createLookupBy(objs, fields) {
        assert.notEqual(fields.length, 0);
        const by = {};
        const grouped = groupBy(objs, e => e[fields[0]]);
        for (const [groupName, group] of Object.entries(grouped)) {
            if (fields.length === 1) {
                assert.equal(group.length, 1);
                by[groupName] = group[0];
            } else {
                by[groupName] = createLookupBy(group, fields.slice(1));
            }
        }
        return by;
    }

    async function parseSimpleMakeFile(path) {
        const contents = await fsp.readFile(path, "utf-8");
        const statements = contents
            .replaceAll("\\\n", "")
            .split("\n")
            .map(line => {
                const match = /:=?/.exec(line);
                if (!match) {
                    return;
                }
                const [matched] = match;
                const {index, input, groups} = match;
                const type = {
                    ":": "rule",
                    ":=": "def",
                }[matched];
                const name = line.slice(0, index).trim();
                const value = line.slice(index + matched.length).trim();
                const values = value.split(/\s+/);
                return {
                    type,
                    name,
                    value,
                    values,
                    push(...values) {
                        this.values.push(...values);
                        this.value += " " + values.join(" ");
                    },
                };
            })
            .filter(Boolean)
            ;
        const by = createLookupBy(statements, ["type", "name"]);
        return by;
    }

    function logArgs(args) {
        const n = 10;
        if (args.length > n * 2) {
            args = [...args.slice(0, 5), "...", ...args.slice(-5)];
        }
        console.log(args.join(" "));
    }

    async function spawn(options) {
        const {args} = options;
        logArgs(args);
        const child = childProcess.spawn(args[0], args.slice(1), options);
        child.wait = () => new Promise((resolve, reject) => {
            child.on("exit", (code, signal) => {
                ((signal || code !== 0) ? reject : resolve)({code, signal});
            });
        });
        child.status = () => new Promise((resolve) => {
            child.on("exit", (code, signal) => resolve({code, signal}))
        });
        return new Promise((resolve, reject) => {
            child.on("spawn", () => resolve(child));
            child.on("error", (e) => reject(e));
        });
    }

    async function canAccess(path, mode) {
        try {
            await fsp.access(path, mode);
            return true;
        } catch {
            return false;
        }
    }

    async function tryStat(path) {
        try {
            return await fsp.stat(path);
        } catch {
            return;
        }
    }

    async function watchKernelFile({outputFile, extraArgs = []}) {
        const {dir, base} = pathLib.parse(outputFile);
        const [cmdPath, depsPath] = ["cmd", "d"]
            .map(ext => pathLib.join(dir, `.${base}.${ext}`))
            ;
        const cmds = await parseSimpleMakeFile(cmdPath);
        ["cmd", "source", "deps"].forEach(prefix => {
            const {def} = cmds;
            def[prefix] = name => def[`${prefix}_${name}`]
                ?? def[`${prefix}_${pathLib.resolve(name)}`];
        });
        const cmdArgs = cmds.def.cmd(outputFile);

        async function runCommandWithExtraArgs({extraArgs, reason}) {
            console.log();
            console.log(`${reason}: ${outputFile}`);
            const child = await spawn({
                args: [
                    ...cmdArgs.values,
                    ...extraArgs.map(arg => {
                        if (arg === "{}") {
                            return cmds.def.source(outputFile).value;
                        }
                        return arg;
                    }),
                ],
                stdio: ['inherit', 'inherit', 'inherit'],
                shell: true,
            });
            const {code, signal} = await child.status();
            if (signal) {
                console.warn(`killed by signal ${signal}: ${outputFile}`);
            }
            if (code !== 0) {
                console.warn(`failed with status ${code}: ${outputFile}`);
            } else {
                console.log(`succeeded: ${outputFile}`);
            }
            console.log();
        }

        let hasDepsFile = await canAccess(depsPath, fs.constants.R_OK);
        if (!hasDepsFile) {
            if (cmdArgs.value.includes(pathLib.basename(depsPath))) {
                await runCommandWithExtraArgs({
                    extraArgs: [],
                    reason: "building once for dependencies",
                });
                hasDepsFile = true;
            }
        }
        const depPaths = await (async () => {
            if (hasDepsFile) {
                const deps = await parseSimpleMakeFile(depsPath);
                return deps.rule[base].values;
            } else {
                const src = cmds.def.source(outputFile);
                return src === undefined ? [] : [src];
            }
        })();
        // console.log({cmds, cmdArgs, depPaths});

        async function runCommand() {
            await runCommandWithExtraArgs({
                extraArgs,
                reason: "rebuilding",
            });
        }

        const debounceTime = 100; // ms

        let currentCommand = null;
        let lastTime = 0.0; // ms
        let commandWaiting = false;

        function run() {
            lastTime = performance.now();
            return runCommand()
                .catch(console.error)
                .finally(() => {
                    if (commandWaiting) {
                        commandWaiting = false;
                    } else {
                        currentCommand = null;
                    }
                });
        }

        function onEvent(eventType, fileName) {
            console.log({eventType, fileName});
            if (commandWaiting) {
                // already waiting for next command, no need to repeat
                return;
            }
            if (currentCommand) {
                const now = performance.now();
                if (now - lastTime < debounceTime) {
                    return;
                }
                commandWaiting = true;
                currentCommand.then(run);
            } else {
                currentCommand = run();
            }
        }

        depPaths.forEach(depPath => fs.watch(depPath, {}, onEvent));
        console.log(`watching for changes to rebuild: ${outputFile}`);
        currentCommand = run();
    }

    async function ensureUnlinked(path) {
        try {
            await fsp.unlink(path);
            console.log(`removed: ${path}`);
        } catch {}
    }

    function cd(dir) {
        const relativeDir = pathLib.relative(".", dir);
        if (relativeDir == "") {
            return;
        }
        console.log(`cd ${relativeDir}`);
        process.chdir(relativeDir);
    }

    async function main() {
        const relativeDir = `/lib/modules/${os.release()}/build`;
        cd("{{just_dir}}");
        const {outputs, extraArgs} = (() => {
            const args = "{{args}}";
            // console.log({args});
            const [outputs, extraArgs] = args
                .split(" -- ")
                .map(s => s === "" ? [] : s.split(" "))
                ;
            return {
                outputs: outputs.map(path => {
                    if (!path.startsWith("./")) {
                        return path;
                    }
                    const relativePath = pathLib.relative(relativeDir, path);
                    if (relativePath === "") {
                        // "" is not a valid path
                        return path;
                    } else {
                        return relativePath;
                    }
                }),
                extraArgs: extraArgs ?? [],
            };
        })();
        console.log({outputs, extraArgs});
        cd(relativeDir);
        const outputFiles = (await Promise.all(outputs.map(async path => {
            const stats = await fsp.stat(path);
            if (!stats?.isDirectory()) {
                return path;
            }
            // only do a shallow search, no recursion
            const dir = path;
            const files = await fsp.readdir(dir);
            return files
                .map(file => {
                    const start = ".";
                    const end = ".cmd";
                    const i = file.indexOf(start);
                    if (i === -1) {
                        return;
                    }
                    const j = file.lastIndexOf(end);
                    if (j === -1) {
                        return;
                    }
                    const outputFile = file.slice(i + start.length, j);
                    return pathLib.join(dir, outputFile);
                })
                .filter(Boolean)
                ;
        }))).flat();
        console.log({outputFiles});
        async function deleteOutputFiles() {
            return;
            await Promise.all(outputFiles.map(ensureUnlinked));
        }
        function onSignal() {
            (async () => {
                await deleteOutputFiles();
                process.exit();
            })();
        }

        process.on("beforeExit", deleteOutputFiles);
        [
            "SIGINT",
            "SIGQUIT",
            "SIGTERM",
        ].forEach(signal => process.on(signal, onSignal));

        await Promise.all(outputFiles.map(outputFile => watchKernelFile({
            outputFile,
            extraArgs,
        })));
    }

    main().catch(e => {
        console.error(e);
        process.exit(1);
    });

watch-pantry *args: (watch-kernel-files "./mypantry.o" "--" args)

watch-format-pantry:
    watchexec 'just make format_disk_as_pantryfs'

symlink-in-dir dir_ target link *args:
    cd "{{dir_}}" && ln {{args}} "{{target}}" "{{link}}"

symlink-readmes:
    fd '^README.txt$' --exec just symlink-in-dir '{//}' '{/}' '{/.}.md' --symbolic --force

setup: symlink-readmes reinstall-kedr fmt compile-commands make check-patch

test-ext2-no-sudo:
    dd if=/dev/zero of=./ext2.img bs=1024 count=100

explore-ext2:
    #!/usr/bin/env bash
    set -euox pipefail

    img=./ext2.img
    mnt=mnt

    dd if=/dev/zero of="${img}" bs=1024 count=100
    sudo modprobe loop
    device="$(sudo losetup --find --show ext2.img)"
    sudo mkfs --type ext2 "${device}"
    sudo mkdir -p "${mnt}"
    sudo mount "${device}" "${mnt}"
    cd mnt
    ls -al
    sudo mkdir sub2
    ls -al
    cd sub2
    ls -al
    sudo mkdir sub2.1
    ls -al
    sudo touch file2.1
    ls -al
    cd ../../
    sudo umount "${mnt}"
    sudo losetup --find
    sudo losetup --detach "${device}"
    sudo losetup --find
    ls -al "${mnt}"

    rmdir "${mnt}"
    rm "${img}"

explore-pantry: (make "format_disk_as_pantryfs")
    #!/usr/bin/env bash
    set -euox pipefail

    img=~/pantry_disk.img
    mnt="/mnt/pantry"
    mod="ref/pantry-x86.ko"

    dd bs=4096 count=200 if=/dev/zero of="${img}"
    device="$(sudo losetup --find --show ~/pantry_disk.img)"
    sudo ./format_disk_as_pantryfs "${device}"
    sudo insmod ref/pantry-x86.ko
    sudo mkdir -p "${mnt}"
    sudo mount -t pantryfs "${device}" "${mnt}"

    # TODO

    sudo umount "${mnt}"
    sudo rmdir "${mnt}"
    sudo rmmod pantry
    sudo losetup --detach "${device}"
    rm "${img}"

part0: explore-ext2 explore-pantry
