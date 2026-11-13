with open("src/m3_launcher.cpp", "r", encoding="utf-8") as f:
    src = f.read()

start = src.find("        } else if (page == Page::Account) {")
end = src.find("        } else {", start)
assert start != -1 and end != -1

acc_block = src[start:end]
src = src[:start] + "        } else {" + src[end + len("        } else {"):]

kvis = src.find("            constexpr int kVisible = 7;")
mods_else = src.rfind("        } else {", 0, kvis)
assert mods_else != -1

src = src[:mods_else] + acc_block + "        } else {" + src[mods_else + len("        } else {"):]

with open("src/m3_launcher.cpp", "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("Moved Account draw block to Mods branch cleanly!")
