# Third-party notices

PlayerLogin.dll (ASA and ASE builds) includes code from the projects below. Their licenses require these
notices to accompany copies of the plugin.

The AsaApi and ArkServerApi (AseApi) server APIs are not included in this repository or in release
builds; they are git submodules that carry their own LICENSE files. fmt is also compiled in, and needs no
notice for binary distribution under its license's exception.

## nlohmann/json 3.10.2

https://github.com/nlohmann/json

Compiled in through the AsaApi / ArkServerApi headers.

MIT License

Copyright (c) 2013-2019 Niels Lohmann <http://nlohmann.me>.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## MariaDB Connector/C (libmariadb) 3.4.8

https://github.com/mariadb-corporation/mariadb-connector-c

Statically linked into the plugin DLL (built through vcpkg, port `libmariadb` 3.4.8). It is licensed under
the GNU Lesser General Public License version 2.1; the full license text is in `LICENSE-LGPL-2.1.txt`,
which accompanies this file in the repository and in every release.

- Source code of this version of the library: https://github.com/mariadb-corporation/mariadb-connector-c
  (tag `v3.4.8`), as built by the vcpkg `libmariadb` port.
- To use a modified version of the library, rebuild the plugin from this repository's source: the
  complete source and Visual Studio project files are published here, and the library version is
  pinned through `vcpkg.json` (`builtin-baseline`). Point the build at your modified library and relink.

## zlib 1.3.1

https://zlib.net

Statically linked as a dependency of libmariadb.

```
Copyright notice:

 (C) 1995-2022 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```
