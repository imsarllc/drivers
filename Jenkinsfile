

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
}
