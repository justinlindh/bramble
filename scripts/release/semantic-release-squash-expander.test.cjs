// Smoke test for scripts/release/semantic-release-squash-expander.cjs.
// Run with: node scripts/release/semantic-release-squash-expander.test.cjs

const assert = require("assert");
const { _expandCommits, _splitSquashBullets } = require("./semantic-release-squash-expander.cjs");

function test(name, fn) {
    try {
        fn();
        console.log(`PASS ${name}`);
    } catch (e) {
        console.error(`FAIL ${name}`);
        console.error(e);
        process.exitCode = 1;
    }
}

test("non-squash commit is left untouched", () => {
    const commit = {
        hash: "abc123",
        message: "fix(firmware): handle NULL packet\n\nDetail body line one.\nDetail body line two.",
        subject: "fix(firmware): handle NULL packet",
        body: "Detail body line one.\nDetail body line two.",
    };
    const out = _expandCommits([commit]);
    assert.deepStrictEqual(out, [commit]);
});

test("commit with bullets that are not conventional commits is left untouched", () => {
    const commit = {
        hash: "abc123",
        message:
            "docs: update README\n\nChanges:\n* Cleaned up the install steps\n* Added a troubleshooting section",
    };
    const out = _expandCommits([commit]);
    assert.strictEqual(out.length, 1);
    assert.strictEqual(out[0].message, commit.message);
});

test("github squash with two bullets expands into two virtual commits", () => {
    const commit = {
        hash: "deadbeef",
        author: { name: "Alice" },
        message:
            "feat(webapp): DM picker journey (#42)\n\n* fix(webapp): correct SAS verification prompt\n\nThe picker still showed the stale one-sided session banner.\nDetails about the fix.\n\n* feat(protocol): bump handshake wire version\n\nThe old handshake could silently drop re-keyed sessions.\n",
    };
    const out = _expandCommits([commit]);
    assert.strictEqual(out.length, 2);
    assert.strictEqual(out[0].subject, "fix(webapp): correct SAS verification prompt");
    assert.strictEqual(out[0].header, "fix(webapp): correct SAS verification prompt");
    assert.strictEqual(
        out[0].body,
        "The picker still showed the stale one-sided session banner.\nDetails about the fix.",
    );
    assert.strictEqual(out[1].subject, "feat(protocol): bump handshake wire version");
    assert.strictEqual(out[1].body, "The old handshake could silently drop re-keyed sessions.");
    // Non-message fields propagate (hash, author, etc.) so changelog links work.
    assert.strictEqual(out[0].hash, "deadbeef");
    assert.strictEqual(out[1].hash, "deadbeef");
    assert.strictEqual(out[0].author.name, "Alice");
});

test("squash with single bullet still expands", () => {
    const commit = {
        hash: "1234",
        message: "fix(firmware): minor (#42)\n\n* fix(firmware): handle NULL packet\n\nbody",
    };
    const out = _expandCommits([commit]);
    assert.strictEqual(out.length, 1);
    assert.strictEqual(out[0].subject, "fix(firmware): handle NULL packet");
    assert.strictEqual(out[0].body, "body");
});

test("split returns null for non-bullet body", () => {
    assert.strictEqual(_splitSquashBullets("subject\n\nbody only"), null);
    assert.strictEqual(_splitSquashBullets("subject only"), null);
    assert.strictEqual(_splitSquashBullets(""), null);
});

test("split recognizes bare type without scope", () => {
    const msg = "feat: cross-cutting (#9)\n\n* fix: nullguard\n\nfoo\n\n* feat: shiny\n\nbar";
    const bullets = _splitSquashBullets(msg);
    assert.strictEqual(bullets.length, 2);
    assert.ok(bullets[0].startsWith("fix: nullguard"));
    assert.ok(bullets[1].startsWith("feat: shiny"));
});

test("split recognizes breaking marker", () => {
    const msg = "feat: rewrites (#9)\n\n* feat(protocol)!: drop v1 endpoint\n\nbody";
    const bullets = _splitSquashBullets(msg);
    assert.strictEqual(bullets.length, 1);
    assert.ok(bullets[0].startsWith("feat(protocol)!: drop v1 endpoint"));
});

test("split recognizes multi-scope", () => {
    const msg =
        "feat(firmware,webapp): re-anchor (#311)\n\n* feat(firmware,webapp): re-anchor release\n\nDetails";
    const bullets = _splitSquashBullets(msg);
    assert.strictEqual(bullets.length, 1);
    assert.ok(bullets[0].startsWith("feat(firmware,webapp): re-anchor release"));
});

// Wiring tests: mock the wrapped plugins via pluginConfig._wrapped and
// verify that analyzeCommits / generateNotes forward an expanded commit
// list and pass through return values.
const expander = require("./semantic-release-squash-expander.cjs");

test("analyzeCommits forwards expanded commits to wrapped commit-analyzer", async () => {
    let receivedCommits = null;
    const mockAnalyzer = {
        async analyzeCommits(cfg, ctx) {
            receivedCommits = ctx.commits;
            return "minor";
        },
    };
    const ctx = {
        commits: [
            {
                hash: "deadbeef",
                message:
                    "feat(webapp): squash subject (#1)\n\n* fix(firmware): real fix\n\nbody one\n\n* feat(protocol): real feat\n\nbody two",
            },
        ],
    };
    const out = await expander.analyzeCommits({ _wrapped: { commitAnalyzer: mockAnalyzer } }, ctx);
    assert.strictEqual(out, "minor");
    assert.strictEqual(receivedCommits.length, 2);
    assert.strictEqual(receivedCommits[0].subject, "fix(firmware): real fix");
    assert.strictEqual(receivedCommits[1].subject, "feat(protocol): real feat");
});

test("generateNotes forwards expanded commits to wrapped notes-generator", async () => {
    let receivedCommits = null;
    const mockNotes = {
        async generateNotes(cfg, ctx) {
            receivedCommits = ctx.commits;
            return "## Release Notes\n";
        },
    };
    const ctx = {
        commits: [
            {
                hash: "abc",
                message: "feat: subj\n\n* fix(firmware): nullguard\n\nguarded the foo",
            },
        ],
    };
    const out = await expander.generateNotes({ _wrapped: { releaseNotesGenerator: mockNotes } }, ctx);
    assert.strictEqual(out, "## Release Notes\n");
    assert.strictEqual(receivedCommits.length, 1);
    assert.strictEqual(receivedCommits[0].subject, "fix(firmware): nullguard");
});

test("missing _wrapped throws a helpful error", async () => {
    await assert.rejects(
        () => expander.analyzeCommits({}, { commits: [] }),
        /pluginConfig\._wrapped\.commitAnalyzer is required/,
    );
    await assert.rejects(
        () => expander.generateNotes({}, { commits: [] }),
        /pluginConfig\._wrapped\.releaseNotesGenerator is required/,
    );
});

// ── inferPathScopes tests ───────────────────────────────────────────

const { _inferPathScopes } = require("./semantic-release-squash-expander.cjs");
const { execSync } = require("child_process");

// These tests need a real git repo. If we're not in one, skip them.
let inGitRepo = false;
try {
    execSync("git rev-parse --git-dir", { encoding: "utf8" });
    inGitRepo = true;
} catch {}

if (inGitRepo) {
    test("inferPathScopes injects synthetic commit when files match but scope does not", () => {
        // Use the actual HEAD commit and a pathScopes map that will match
        // something in the repo. We don't know what files HEAD touched, so
        // use a prefix that matches the test file itself.
        const head = execSync("git rev-parse HEAD", { encoding: "utf8" }).trim();
        const files = execSync(`git diff-tree --no-commit-id --name-only -r ${head}`, {
            encoding: "utf8",
        }).trim().split("\n").filter(Boolean);

        if (files.length === 0) {
            console.log("SKIP inferPathScopes inject (HEAD has no files)");
            return;
        }

        // Pick a prefix from the first file
        const prefix = files[0].split("/")[0] + "/";
        const commits = [{
            hash: head,
            subject: "feat(unrelated): something",
            header: "feat(unrelated): something",
            message: "feat(unrelated): something",
        }];

        const result = _inferPathScopes(commits, { [prefix]: "testscope" });
        assert.strictEqual(result.length, 2, "should have original + synthetic");
        assert.strictEqual(result[0].subject, "feat(unrelated): something");
        assert.ok(result[1].subject.includes("testscope"), "synthetic has inferred scope");
        assert.ok(result[1].subject.startsWith("feat("), "preserves original type");
    });

    test("inferPathScopes does not inject when scope already matches", () => {
        const head = execSync("git rev-parse HEAD", { encoding: "utf8" }).trim();
        const commits = [{
            hash: head,
            subject: "feat(firmware): something",
            header: "feat(firmware): something",
            message: "feat(firmware): something",
        }];

        const result = _inferPathScopes(commits, { "main/": "firmware" });
        assert.strictEqual(result.length, 1, "no synthetic when scope already present");
    });

    test("inferPathScopes is a no-op when pathScopes is empty or absent", () => {
        const commits = [{ hash: "abc", subject: "feat: x", header: "feat: x", message: "feat: x" }];
        assert.deepStrictEqual(_inferPathScopes(commits, {}), commits);
        assert.deepStrictEqual(_inferPathScopes(commits, undefined), commits);
        assert.deepStrictEqual(_inferPathScopes(commits, null), commits);
    });
}

// ── release-rule scope-gating regression ────────────────────────────
//
// Loads each real .releaserc.<component>.cjs and runs its releaseRules
// through the same squash-expander + @semantic-release/commit-analyzer path
// the release workflow uses. Guards the bug where a { scope: '*', release:
// false } catch-all shadowed every in-scope rule (commit-analyzer ranks a
// matched release:false as the highest-priority match), so no component ever
// cut a release. Asserts, per component, that an in-scope fix/feat/perf/
// breaking releases at the right level while an out-of-scope or non-releasing
// commit does not, and that an in-scope bullet inside a cross-component
// squash body still releases.

async function runReleaseRuleRegression() {
    const path = require("path");
    const REPO_ROOT = path.resolve(__dirname, "..", "..");
    const COMPONENTS = ["firmware", "webapp", "protocol", "sim"];

    // Loading a real .releaserc pulls in @semantic-release/commit-analyzer and
    // @semantic-release/release-notes-generator. Those live in the ephemeral
    // node_modules the release workflow installs, not in a bare dev checkout.
    // When they are absent, skip loudly rather than fail; CI installs them and
    // runs the real assertions (see the "Release config" gate in quality.yml).
    try {
        require(path.join(REPO_ROOT, ".releaserc.firmware.cjs"));
    } catch (e) {
        if (e && e.code === "MODULE_NOT_FOUND") {
            console.log(
                "SKIP release-rule scope-gating regression (@semantic-release/* not installed)",
            );
            return;
        }
        throw e;
    }

    const expander = require("./semantic-release-squash-expander.cjs");

    async function releaseFor(component, message) {
        const cfg = require(path.join(REPO_ROOT, `.releaserc.${component}.cjs`));
        const pluginConfig = cfg.plugins[0][1];
        return expander.analyzeCommits(pluginConfig, {
            commits: [{ hash: "regression", message }],
            logger: { log() {} },
        });
    }

    for (const c of COMPONENTS) {
        // Any other real scope: exercises the out-of-scope and cross-component
        // paths against a scope that is genuinely not this component's.
        const other = c === "webapp" ? "firmware" : "webapp";
        const crossSquash = `feat(${other}): squash subject (#1)\n\n* fix(${c}): real in-scope fix\n\nbody`;
        const expectations = [
            [`fix(${c}): in-scope fix`, "patch"],
            [`feat(${c}): in-scope feat`, "minor"],
            [`perf(${c}): in-scope perf`, "patch"],
            [`feat(${c})!: in-scope breaking change`, "major"],
            [`fix(${other}): out-of-scope fix`, null],
            ["docs: no scope at all", null],
            [`chore(${c}): non-releasing type`, null],
            [crossSquash, "patch"],
        ];
        for (const [message, expected] of expectations) {
            const actual = await releaseFor(c, message);
            const label = `${c}: ${JSON.stringify(message.split("\n")[0])} => ${JSON.stringify(
                actual,
            )} (expect ${JSON.stringify(expected)})`;
            if (actual === expected) {
                console.log(`PASS ${label}`);
            } else {
                console.error(`FAIL ${label}`);
                process.exitCode = 1;
            }
        }
    }

    // Pin the KNOWN, PRE-EXISTING multi-scope limitation so a future change to
    // it is deliberate. The specific rules match scope with plain micromatch
    // (`scope: 'firmware'`), which compares against the whole scope string, so
    // a comma-joined scope like `firmware,webapp` matches neither the specific
    // firmware rule nor the negated `!(firmware)` catch-all and yields no
    // release. This is unchanged by this fix (the old `scope: '*'` catch-all
    // behaved identically here) and is orthogonal to the catch-all shadowing
    // bug; the writerOpts.transform handles multi-scope for release NOTES, but
    // the release DECISION does not. If multi-scope release decisions are ever
    // wanted, change the rules deliberately and update this assertion.
    // Scope `ui` is DEVICE-UI code (components/ui_graphics, the T-Deck LVGL
    // screens): it compiles into the firmware image, so ui-scoped changes cut
    // FIRMWARE releases and stay out-of-scope for every other component. Pinned
    // here so the mapping cannot silently regress in either direction.
    const uiExpectations = [
        ["firmware", "fix(ui): device UI fix", "patch"],
        ["firmware", "feat(ui): device UI feature", "minor"],
        ["firmware", "perf(ui): device UI perf", "patch"],
        ["firmware", "feat(ui)!: device UI breaking change", "major"],
        ["firmware", "feat(webapp): squash subject (#2)\n\n* fix(ui): bullet fix\n\nbody", "patch"],
        ["webapp", "fix(ui): device UI fix", null],
        ["sim", "fix(ui): device UI fix", null],
        ["protocol", "fix(ui): device UI fix", null],
    ];
    for (const [component, message, expected] of uiExpectations) {
        const actual = await releaseFor(component, message);
        const label = `${component}: ${JSON.stringify(message.split("\n")[0])} => ${JSON.stringify(
            actual,
        )} (expect ${JSON.stringify(expected)})`;
        if (actual === expected) {
            console.log(`PASS ${label}`);
        } else {
            console.error(`FAIL ${label}`);
            process.exitCode = 1;
        }
    }

    const multiScope = await releaseFor("firmware", "feat(firmware,webapp): shared change");
    if (multiScope === null) {
        console.log('PASS firmware: multi-scope "feat(firmware,webapp)" => null (known limitation)');
    } else {
        console.error(
            `FAIL firmware: multi-scope "feat(firmware,webapp)" => ${JSON.stringify(
                multiScope,
            )} (expected null, known limitation)`,
        );
        process.exitCode = 1;
    }
}

runReleaseRuleRegression()
    .catch((e) => {
        console.error("FAIL release-rule scope-gating regression threw");
        console.error(e);
        process.exitCode = 1;
    })
    .finally(() => {
        if (process.exitCode) {
            process.exit(process.exitCode);
        }
    });
