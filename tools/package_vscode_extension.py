#!/usr/bin/env python3
"""Packages an extension folder as a .vsix (stdlib only, no Node).

A .vsix is a zip holding [Content_Types].xml, extension.vsixmanifest and
the extension files under extension/. The calcu1600-debug extension is
plain JS without dependencies or a build step, so this is all vsce would
do for it. vsce fetched through npx follows its newest release and
dependencies, which drop older Node versions (e.g. Node 18 on Debian 12 /
Ubuntu 24.04).

Usage: package_vscode_extension.py <extension dir> <output .vsix>
"""
import json
import os
import sys
import zipfile
from xml.sax.saxutils import escape, quoteattr

CONTENT_TYPES = {
    ".json": "application/json",
    ".js": "application/javascript",
    ".md": "text/markdown",
    ".vsixmanifest": "text/xml",
}


def manifest(pkg):
    attr = lambda key: quoteattr(str(pkg.get(key, "")))
    return f"""<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id={attr("name")} Version={attr("version")} Publisher={attr("publisher")} />
    <DisplayName>{escape(pkg.get("displayName", pkg["name"]))}</DisplayName>
    <Description xml:space="preserve">{escape(pkg.get("description", ""))}</Description>
    <Categories>{escape(",".join(pkg.get("categories", [])))}</Categories>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value={quoteattr(pkg["engines"]["vscode"])} />
      <Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace" />
      <Property Id="Microsoft.VisualStudio.Code.ExecutesCode" Value="true" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true" />
  </Assets>
</PackageManifest>
"""


def main(src, out):
    with open(os.path.join(src, "package.json"), encoding="utf-8") as f:
        pkg = json.load(f)
    files = sorted(
        os.path.relpath(os.path.join(root, name), src)
        for root, dirs, names in os.walk(src)
        for name in names
        if not name.startswith(".")
    )
    exts = sorted({os.path.splitext(p)[1] for p in files} | {".vsixmanifest"})
    types = "".join(
        f'<Default Extension="{e}" ContentType="{CONTENT_TYPES.get(e, "application/octet-stream")}"/>'
        for e in exts if e
    )
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="utf-8"?>\n'
                   '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   f"{types}</Types>")
        z.writestr("extension.vsixmanifest", manifest(pkg))
        for p in files:
            z.write(os.path.join(src, p), "extension/" + p.replace(os.sep, "/"))
    print(f"Packaged {out} ({len(files)} files)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
