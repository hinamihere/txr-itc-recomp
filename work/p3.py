p = r'C:\Users\日南\Documents\txr-itc-recomp\tools\XenonRecomp\XenonRecomp\recompiler.cpp'
s = open(p, encoding='utf-8').read()
a = s.index("        for (const auto& [switchAddress, table] : config.switchTables)\n        {\n            Function* owner")
b = s.index("    }\n}\n\nbool Recompiler::Recompile(")
new = r'''        for (const auto& [switchAddress, table] : config.switchTables)
        {
            for (auto& owner : functions)
            {
                if (!(owner.base <= switchAddress && switchAddress < owner.base + owner.size))
                    continue;

                for (uint32_t label : table.labels)
                {
                    if (label < owner.base || label < owner.base + owner.size)
                        continue;

                    const Function* target = nullptr;
                    for (const auto& fn : functions)
                    {
                        if (fn.base <= label && label < fn.base + fn.size && (!target || fn.base > target->base))
                            target = &fn;
                    }
                    if (!target)
                        continue;

                    owner.size = target->base + target->size - owner.base;
                    changed = true;
                }
            }
        }
'''
s = s[:a] + new + s[b:]
open(p, 'w', encoding='utf-8').write(s)
print('ok')
