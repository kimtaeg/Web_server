/*
 * mime.h — 파일 확장자 → Content-Type.
 *
 * 정적 테이블 + 선형 탐색. 항목이 수십 개라 해시보다 단순한 쪽이 낫고,
 * 테이블은 읽기 전용이라 여러 워커가 락 없이 동시에 써도 안전하다.
 */
#ifndef MIME_H
#define MIME_H

#define MIME_DEFAULT "application/octet-stream"

/*
 * 마지막 경로 구성요소의 마지막 '.' 뒤를 확장자로 본다 (대소문자 무시).
 * 확장자가 없거나, 숨김 파일(.bashrc)이거나, 모르는 확장자면 MIME_DEFAULT.
 * 텍스트 계열은 "; charset=utf-8" 이 붙는다. 반환값은 정적 문자열(해제 금지).
 */
const char *mime_type_for_path(const char *path);

#endif /* MIME_H */
