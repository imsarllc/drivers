node('kernel')
{
  stage('Checkout')
  {
    cleanWs()
    checkout scm
  }
  stage('Build 2013.4')
  {
    sh('make 2013.4')
  }
  stage('Build 2016.4')
  {
    sh('''\
      #!/bin/bash
      source /etc/profile.d/rvm.sh; 
      type rvm | head -n 1; 
      rvm use
      make 2016.4'''.stripIndent())
  }
  stage('Archive')
  {
    archiveArtifacts artifacts: 'build/**', fingerprint: true
  }
  if(env.BRANCH_NAME == "master")
  {
    stage('Upload')
    {
      def uploadSpec = """{
        "files": [
          {
            "pattern": "*.deb",
            "target": "fpga-deb-nightly/pool/",
            "props": "deb.distribution=xenial;deb.distribution=bionic;deb.component=contrib;deb.architecture=armhf"
          }
        ]
      }"""
      rtUpload(
        serverId: 'artifactory',
        spec: uploadSpec,
      )
      rtBuildInfo(
        captureEnv: true,
        maxBuilds: 10,
        deleteBuildArtifacts: true
      )
      rtPublishBuildInfo(
        serverId: 'artifactory',
      )
      rtAddInteractivePromotion(
        serverId: 'artifactory',
        displayName: 'Promote me please',
        sourceRepo: 'fgpa-deb-nightly',
        targetRepo: 'fpga-deb-release',
        comment: 'Why are you promoting this?',
        status: 'Released',
        failFast: true,
        copy: true
      )
    }
  }
}
