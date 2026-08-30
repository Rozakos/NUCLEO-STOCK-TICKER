// CI for the STM32F746G-DISCO stock ticker.
//
// Unlike the PlatformIO repos, this is an STM32CubeIDE project: the build
// description lives in .cproject (Eclipse CDT XML) and there is no Makefile.
// So CI drives CubeIDE's headless CDT builder, which is installed on the
// Jenkins controller at /opt/st/stm32cubeide_1.19.0.
//
// Two things about that builder shape this file:
//
//  1. ITS EXIT CODE CANNOT BE TRUSTED. On a verified-good build it linked a
//     valid 11 MB .elf and still exited 1, printing only its own launcher
//     arguments with no compile log and an empty CDT error log. So the build
//     step tolerates a non-zero exit and the pipeline decides pass/fail by
//     checking that a FRESH .elf actually exists. That is a real signal; the
//     exit code is not.
//
//  2. The Debug config does not emit .bin or .hex, only .elf/.list/.map. Those
//     are generated explicitly below with objcopy so there is something
//     flashable to archive.

pipeline {
  agent any

  options {
    timestamps()
    timeout(time: 45, unit: 'MINUTES')
    disableConcurrentBuilds()
    buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
  }

  environment {
    CUBEIDE = '/opt/st/stm32cubeide_1.19.0/stm32cubeide'
    // Eclipse workspace kept outside the workspace dir so cleanWs() does not
    // force a full re-import and rebuild every time.
    CUBE_WS = "${JENKINS_HOME}/cube-workspace"
    PROJECT = 'NUCLEO-STOCK-TICKER'
  }

  stages {
    stage('Checkout') {
      steps {
        checkout scm
        sh 'git --no-pager log -1 --oneline'
      }
    }

    stage('Stub secrets.h') {
      steps {
        // Core/Inc/app/secrets.h is gitignored (API base, bearer token,
        // symbols), so a clean clone cannot compile. The committed
        // secrets.h.example is the template; CI uses it exactly as a human
        // would. This proves the code COMPILES - the artifact carries
        // placeholder values and is not a drop-in image for real use.
        sh '''
          if [ ! -f Core/Inc/app/secrets.h ]; then
            cp Core/Inc/app/secrets.h.example Core/Inc/app/secrets.h
            echo "secrets.h stubbed from secrets.h.example"
          fi
        '''
      }
    }

    stage('Build (CubeIDE headless)') {
      steps {
        // Remove prior output so the freshness check below cannot be fooled by
        // a stale .elf from an earlier build.
        sh 'rm -rf Debug'
        sh '''
          set +e
          "$CUBEIDE" --launcher.suppressErrors -nosplash \
            -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
            -data "$CUBE_WS" \
            -import "$WORKSPACE" \
            -build "${PROJECT}/Debug" \
            -no-indexer
          echo "CubeIDE headless exit: $? (not used as the pass/fail signal - see header)"
          exit 0
        '''
      }
    }

    stage('Verify build output') {
      steps {
        // This is the actual gate. No .elf means the build genuinely failed,
        // whatever the launcher reported.
        sh '''
          ELF="Debug/${PROJECT}.elf"
          if [ ! -f "$ELF" ]; then
            echo "FAILED: no $ELF produced - the build did not link"
            exit 1
          fi
          echo "linked: $(ls -lh "$ELF" | awk '{print $5}')"
        '''
      }
    }

    stage('Generate bin/hex + size report') {
      steps {
        // The toolchain path is version-pinned by CubeIDE, so resolve it rather
        // than hardcoding the plugin directory - it changes on every IDE update.
        sh '''
          OBJCOPY=$(find /opt/st -name arm-none-eabi-objcopy -type f 2>/dev/null | head -1)
          SIZE=$(find /opt/st -name arm-none-eabi-size -type f 2>/dev/null | head -1)
          cd Debug
          "$OBJCOPY" -O binary "${PROJECT}.elf" "${PROJECT}.bin"
          "$OBJCOPY" -O ihex   "${PROJECT}.elf" "${PROJECT}.hex"
          ls -lh "${PROJECT}".{elf,bin,hex}

          echo ""
          echo "STM32F746NG budget: 1024 KB flash / 320 KB RAM"
          "$SIZE" "${PROJECT}.elf"
          "$SIZE" -A "${PROJECT}.elf" | head -12
        '''
      }
    }

    stage('Archive') {
      steps {
        archiveArtifacts artifacts: "Debug/${PROJECT}.elf,Debug/${PROJECT}.bin,Debug/${PROJECT}.hex,Debug/${PROJECT}.map",
                         fingerprint: true,
                         onlyIfSuccessful: true
      }
    }
  }

  post {
    cleanup {
      cleanWs()
    }
  }
}
