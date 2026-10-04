#pragma once

class QQmlEngine;

void configureEmbeddedQmlEngine(QQmlEngine& engine);
int runApplication(int argc, char* argv[]);
int runCiSmokeTest(int argc, char* argv[]);
