

node('watson')
{
  stage('Checkout')
  {
    checkout scm
  }
  stage('Build 2013.4')
  {
    sh('make 2013.4')
  }
  stage('Build 2016.4')
  {
    sh('make 2016.4')
  }
  stage('Archive')
  {
    archiveArtifacts artifacts: 'build/*', fingerprint: true
  }
}
