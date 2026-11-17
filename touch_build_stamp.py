# 2.1.7: the "build <date> <time>" shown in the web header comes from __DATE__/__TIME__ in
# webservice.cpp. Touch it before every build so it is always recompiled and the stamp is real.
Import("env")
import os
os.utime(os.path.join(env.subst("$PROJECT_SRC_DIR"), "webservice.cpp"), None)
