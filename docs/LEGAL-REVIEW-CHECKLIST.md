# Legal Review Checklist for Aegisy Agent Workbench

**Task Reference:** OpenSpec 1.5 - Complete legal review for process-level Codex integration, Apache-2.0/MIT notices, Claude public examples, and product branding boundaries

**Status:** Pending legal team approval

**Related Documents:**
- `/Users/cwm/aegisy-app/docs/THIRD-PARTY-COMPONENT-INVENTORY.md` - Component inventory
- `/Users/cwm/aegisy-app/openspec/changes/build-aegisy-agent-workbench/tasks.md` - Task 1.5

---

## 1. Codex Integration Legal Review

### 1.1 Process-Level Integration Model
- [ ] Review and approve the external process integration model where Codex CLI/App Server runs as a separate executable
- [ ] Confirm that external process integration (vs. bundling) meets distribution requirements
- [ ] Verify that the exact-version runtime gate (`codex-cli 0.144.5`) is legally sufficient
- [ ] Approve the adapter boundary and protocol mapping approach

### 1.2 Codex Distribution and Bundling
- [ ] Confirm current External status: Aegisy discovers user/system Codex installation and does not redistribute
- [ ] Review requirement: "Current packages must not claim Codex is bundled"
- [ ] If future bundling is planned, approve requirements for:
  - [ ] Exact package/platform artifact identification
  - [ ] Apache-2.0 license text and NOTICE file inclusion
  - [ ] Transitive and native dependency inventory
  - [ ] Signature/hash verification procedures
  - [ ] Updater compatibility evidence

### 1.3 Codex Licensing and Attribution
- [ ] Verify Codex App Server Apache-2.0 license compliance
- [ ] Review NOTICE file requirements for Apache-2.0 components
- [ ] Approve attribution text for Codex in product documentation
- [ ] Confirm that adapter code (`agent-runtime/crates/aegisy-agentd/src/codex_adapter.rs`) properly attributes Codex

### 1.4 Codex Trademark and Branding
- [ ] Review use of "Codex" name in product materials
- [ ] Approve any Codex branding in UI, documentation, or marketing
- [ ] Confirm compliance with OpenAI trademark guidelines
- [ ] Verify that product does not imply endorsement by OpenAI

---

## 2. Apache-2.0 and MIT License Compliance

### 2.1 Apache-2.0 Components
- [ ] **Codex App Server** (`codex-cli 0.144.5`) - External
  - [ ] License text included in distribution (if bundled)
  - [ ] NOTICE file preserved and included
  - [ ] Attribution requirements met
- [ ] **Aegisy Agent Protocol** (`0.1`) - First-party
  - [ ] Copyright notice present
  - [ ] License file in repository
  - [ ] Schema compatibility evidence maintained
- [ ] **similar** (`3.1.1`) - Compiled into sidecar
  - [ ] License text in THIRD-PARTY-NOTICES.md
  - [ ] Attribution in release bundle
- [ ] **TypeScript Language Server** - External (if bundled in future)
  - [ ] License and notices prepared for bundling scenario
- [ ] **clangd** - External (if bundled in future)
  - [ ] Apache-2.0 WITH LLVM-exception reviewed
  - [ ] LLVM exception text included
- [ ] **OpenSSL** (`3.x`) - Bundled on Windows
  - [ ] Apache-2.0 license compliance
  - [ ] Version and hash documentation

### 2.2 MIT License Components
- [ ] **Monaco Editor** (`0.55.1`) - Bundled
  - [ ] MIT license text included
  - [ ] Component/version record in bundle
- [ ] **xterm.js** (`6.0.0`) - Bundled
  - [ ] MIT license text included
  - [ ] Component/version record in bundle
- [ ] **xterm.js FitAddon** (`0.11.0`) - Bundled
  - [ ] MIT license with xterm.js inventory
- [ ] **esbuild** (`0.28.1`) - Build-time only
  - [ ] Build provenance recorded
  - [ ] Not presented as runtime capability
- [ ] **tree-sitter** components - All compiled into sidecar
  - [ ] tree-sitter Rust bindings (`0.26.11`)
  - [ ] tree-sitter-cpp grammar (`0.23.4`)
  - [ ] tree-sitter-javascript grammar (`0.25.0`)
  - [ ] tree-sitter-python grammar (`0.25.0`)
  - [ ] tree-sitter-rust grammar (`0.24.2`)
  - [ ] tree-sitter-typescript/TSX grammars (`0.23.2`)
  - [ ] Individual grammar copyright/license texts included (not just binding license)
- [ ] **portable-pty** (`0.9.0`) - Compiled into sidecar
  - [ ] MIT license in notices
- [ ] **rusqlite** (`0.40.1`) - Compiled into sidecar
  - [ ] MIT license in notices
  - [ ] SQLite public-domain notice included separately
- [ ] **Sparkle** (`2.9.4`) - Bundled in macOS app
  - [ ] MIT license preserved
  - [ ] Embedded third-party notices from upstream
- [ ] **WinSparkle** (`0.9.3`) - Bundled in Windows app
  - [ ] MIT license with DLL
- [ ] **rust-analyzer** - External (if bundled in future)
  - [ ] MIT OR Apache-2.0 dual license reviewed
- [ ] **Pyright** - External (if bundled in future)
  - [ ] MIT license and Node runtime ownership clarified

### 2.3 Dual-Licensed Components (MIT OR Apache-2.0)
- [ ] **windows-sys** (`0.61.2`) - Compiled into Windows sidecar
  - [ ] License choice documented
  - [ ] Chosen license text included
- [ ] **libc** (`0.2.174`) - Compiled on Unix targets
  - [ ] License choice documented
  - [ ] Chosen license text included
- [ ] **strip-ansi-escapes** (`0.2.1`) - Compiled into sidecar
  - [ ] License choice documented
  - [ ] Transitive `vte` notice included

### 2.4 Other Licenses
- [ ] **ed25519-dalek** (`2.1.1`) - BSD-3-Clause
  - [ ] BSD-3-Clause license text included
  - [ ] Copyright notice preserved
- [ ] **Lucide icons** (`1.24.0`) - ISC
  - [ ] ISC license at `assets/icons/lucide/LICENSE`
- [ ] **Qt** (5.15+ or 6) - LGPL/GPL/Commercial
  - [ ] License basis chosen and documented
  - [ ] Exact version/modules recorded
  - [ ] Relinking/source obligations met (if LGPL)
  - [ ] WebEngine notices included

### 2.5 Consolidated License Files
- [ ] `agent-runtime/THIRD-PARTY-NOTICES.md` synchronized with runtime dependencies
- [ ] Generated release inventory includes all transitive license texts
- [ ] Signed macOS package contains complete NOTICE/license set
- [ ] Signed Windows package contains complete NOTICE/license set
- [ ] All license files match actual bundled components

---

## 3. Claude Public Examples Usage

### 3.1 Source Code Examples
- [ ] Identify any code derived from Claude public examples or documentation
- [ ] Review licensing terms for Claude example code
- [ ] Verify attribution requirements are met
- [ ] Confirm modifications are properly documented

### 3.2 Documentation and Patterns
- [ ] Review use of Claude-provided patterns in documentation
- [ ] Verify compliance with any usage restrictions
- [ ] Approve attribution in developer documentation
- [ ] Confirm examples are properly adapted for Aegisy context

### 3.3 API Integration Examples
- [ ] Review Claude API integration code for licensing
- [ ] Verify that example-derived code meets terms of use
- [ ] Approve any Claude API documentation references
- [ ] Confirm proper attribution in code comments

---

## 4. Product Branding Boundaries

### 4.1 Aegisy Product Name
- [ ] Approve final public product name (currently under review per task 1.2)
- [ ] Review trademark search results
- [ ] Confirm name does not conflict with existing marks
- [ ] Approve product name usage guidelines

### 4.2 Third-Party Branding
- [ ] Review use of "Codex" branding and approve boundaries
- [ ] Review use of "Claude" branding and approve boundaries
- [ ] Verify no unauthorized use of AWS trademarks
- [ ] Confirm no unauthorized use of Anthropic trademarks
- [ ] Verify no unauthorized use of OpenAI trademarks
- [ ] Review Monaco Editor branding (Microsoft)
- [ ] Review Qt branding usage

### 4.3 Component Attribution
- [ ] Approve "Powered by" or similar attribution statements
- [ ] Review component credits in About dialog
- [ ] Verify attribution does not imply endorsement
- [ ] Approve third-party logo usage (if any)

### 4.4 Marketing and Documentation
- [ ] Review product description for trademark compliance
- [ ] Approve feature descriptions mentioning third-party components
- [ ] Verify documentation does not misrepresent relationships
- [ ] Confirm marketing materials respect branding boundaries

---

## 5. Additional Legal Considerations

### 5.1 Export and Cryptography
- [ ] Review cryptographic components for export compliance
  - [ ] ed25519-dalek signature verification
  - [ ] OpenSSL usage
- [ ] Confirm export classification if required
- [ ] Verify compliance with applicable regulations

### 5.2 Source Availability
- [ ] Review source availability requirements for LGPL components (Qt)
- [ ] Confirm relinking procedures documented (if applicable)
- [ ] Verify modification notices for modified components
- [ ] Approve source distribution mechanism (if required)

### 5.3 Planned Components
- [ ] Review legal requirements before ACP integration
- [ ] Approve sandbox implementation selection before bundling
- [ ] Verify language server bundling meets license requirements
- [ ] Confirm all "Planned" components have legal review before integration

### 5.4 Release Gates
- [ ] Verify `agent-runtime/Cargo.lock` is version authority
- [ ] Verify `workbench-web/package-lock.json` is version authority
- [ ] Confirm unknown/unpinned/missing-license components block promotion
- [ ] Approve release inventory generation process

---

## 6. Final Approval

### 6.1 Legal Team Sign-Off
- [ ] All checklist items reviewed and approved
- [ ] Outstanding issues documented with resolution plan
- [ ] Legal counsel approval obtained
- [ ] Approval date and reviewer name recorded

### 6.2 Documentation
- [ ] Legal review results documented
- [ ] Any restrictions or conditions noted
- [ ] Ongoing compliance requirements identified
- [ ] Review schedule for future updates established

---

**Legal Reviewer:** ___________________________  
**Date:** ___________________________  
**Approval Status:** ☐ Approved  ☐ Approved with Conditions  ☐ Rejected  

**Notes:**
