import re
base = r'C:\Users\日南\Documents\txr-itc-recomp\tools\XenonRecomp\XenonUtils'

# image.h: record imports
p = base + r'\image.h'
s = open(p, encoding='utf-8').read()
if 'ImageImport' not in s:
    s = s.replace('#include <set>\n', '#include <set>\n#include <vector>\n', 1)
    s = s.replace('struct Image\n{', '''struct ImageImport
{
    uint32_t address{};     // VA of the import record / thunk in the image
    uint32_t ordinal{};
    bool isFunction{};      // false = variable import (record must receive the variable's address)
    std::string library;
    std::string name;       // "__imp__<Name>" or empty if the ordinal is unknown
};

struct Image
{
    std::vector<ImageImport> imports{};''', 1)
    open(p, 'w', encoding='utf-8').write(s)

# xex.cpp: fill it
p = base + r'\xex.cpp'
s = open(p, encoding='utf-8').read()
if 'image.imports.push_back' not in s:
    old = '''                if (originalData->originalData.type != 0)
                {'''
    new = '''                {
                    auto nameIt = names->find(originalData->originalData.ordinal);
                    image.imports.push_back({ (uint32_t)descriptors[im].firstThunk, (uint32_t)originalData->originalData.ordinal,
                        originalData->originalData.type != 0, std::string(stringTable[i]),
                        nameIt != names->end() ? nameIt->second : "" });
                }

                if (originalData->originalData.type != 0)
                {'''
    assert s.count(old) == 1
    s = s.replace(old, new)
    open(p, 'w', encoding='utf-8').write(s)
print('ok')
