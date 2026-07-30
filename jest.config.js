/** Jest configuration. Main-process code is CommonJS, so no transform is needed. */
module.exports = {
  testEnvironment: 'node',
  testMatch: ['**/__tests__/**/*.test.js'],
  collectCoverageFrom: ['src/main/**/*.js'],
  coverageDirectory: 'coverage',
  moduleFileExtensions: ['js', 'json'],
};
