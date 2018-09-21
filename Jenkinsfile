node('watson')
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
      def server = Artifactory.server 'artifactory'
      def uploadSpec = """{
        "files": [
          {
            "pattern": "*.deb",
            "target": "fpga-deb-nightly/pool/",
            "props": "deb.distribution=xenial;deb.component=contrib;deb.architecture=armhf"
          }
        ]
      }"""
      server.upload(uploadSpec)
    }
  }
}
